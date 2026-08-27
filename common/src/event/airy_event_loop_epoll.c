// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file airy_event_loop_epoll.c
 * @brief Event loop Linux backend: epoll multiplexing and callback dispatch.
 *
 * Linux 平台后端（IO 多路复用域）：fd 注册表、epoll 订阅与 run 主循环
 * 内的回调分发。事件循环核心门面见 airy_event_loop.c；定时器管理见
 * airy_event_timer.c（本后端仅在每轮 poll 后推进定时器）。
 *
 * fd 直接作为 fd_entries 数组索引（fd < max_events）；wakeup 用 eventfd
 * （EFD_NONBLOCK），stop_async 内仅 write 一个 syscall，保持
 * async-signal-safe。
 */

#include "airy_event_loop.h"
#include "airy_event_loop_internal.h"

#include "daemon_errors.h"
#include "airy_memory.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && defined(__linux__)

#include <sys/epoll.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#include "svc_logger.h"

typedef struct {
    int fd;
    uint32_t events;
    airy_event_callback_t cb;
    void *user_data;
    bool level_triggered;
} fd_entry_t;

struct airy_event_loop {
    int epoll_fd;
    int wakeup_fd;
    int max_events;
    struct epoll_event *epoll_events;
    fd_entry_t *fd_entries;
    airy_timer_state_t timers;
    volatile bool running;
    volatile bool stop_requested;
};

airy_timer_state_t *airy_event_loop_timers(airy_event_loop_t *loop)
{
    return loop ? &loop->timers : NULL;
}

static int add_fd_internal(airy_event_loop_t *loop, int fd, uint32_t events,
                           airy_event_callback_t cb, void *user_data, bool level_triggered)
{
    if (!loop || fd < 0 || !cb)
        return AIRY_ERR_INVALID_PARAM;
    if (fd >= loop->max_events) {
        AIRY_LOG_DEBUG("fd=%d exceeds max_events=%d, cannot track callback", fd, loop->max_events);
        return AIRY_ERR_INVALID_PARAM;
    }

    struct epoll_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;

    if (events & AIRY_EVENT_TYPE_READ)
        ev.events |= EPOLLIN;
    if (events & AIRY_EVENT_TYPE_WRITE)
        ev.events |= EPOLLOUT;
    if (!level_triggered)
        ev.events |= EPOLLET;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        if (errno == EEXIST) {
            if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
                AIRY_LOG_DEBUG("epoll_ctl MOD failed for fd=%d: %s", fd, strerror(errno));
                return AIRY_ERR_IO;
            }
        } else {
            AIRY_LOG_DEBUG("epoll_ctl ADD failed for fd=%d: %s", fd, strerror(errno));
            return AIRY_ERR_IO;
        }
    }

    loop->fd_entries[fd].fd = fd;
    loop->fd_entries[fd].events = events;
    loop->fd_entries[fd].cb = cb;
    loop->fd_entries[fd].user_data = user_data;
    loop->fd_entries[fd].level_triggered = level_triggered;

    return 0;
}

airy_event_loop_t *airy_event_loop_create(int max_events)
{
    airy_event_loop_t *loop = (airy_event_loop_t *)AIRY_CALLOC(1, sizeof(airy_event_loop_t));
    if (!loop) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (max_events <= 0)
        max_events = AIRY_EVENT_LOOP_MAX_EVENTS;

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        AIRY_FREE(loop);
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    loop->max_events = max_events;
    loop->epoll_events =
        (struct epoll_event *)AIRY_CALLOC((size_t)max_events, sizeof(struct epoll_event));
    loop->fd_entries = (fd_entry_t *)AIRY_CALLOC((size_t)max_events, sizeof(fd_entry_t));

    if (!loop->epoll_events || !loop->fd_entries) {
        close(loop->epoll_fd);
        AIRY_FREE(loop->epoll_events);
        AIRY_FREE(loop->fd_entries);
        AIRY_FREE(loop);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    loop->wakeup_fd = -1;
#ifdef __linux__
    loop->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wakeup_fd >= 0) {
        struct epoll_event ev;
        __builtin_memset(&ev, 0, sizeof(ev));
        ev.data.fd = loop->wakeup_fd;
        ev.events = EPOLLIN;
        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->wakeup_fd, &ev) < 0) {
            close(loop->wakeup_fd);
            loop->wakeup_fd = -1;
        }
    }
#endif

    airy_timer_init(&loop->timers);

    AIRY_LOG_DEBUG("Event loop created (epoll_fd=%d, wakeup_fd=%d, max_events=%d)", loop->epoll_fd,
              loop->wakeup_fd, max_events);
    return loop;
}

void airy_event_loop_destroy(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    if (loop->wakeup_fd >= 0)
        close(loop->wakeup_fd);
    close(loop->epoll_fd);
    AIRY_FREE(loop->epoll_events);
    AIRY_FREE(loop->fd_entries);
    AIRY_FREE(loop);
}

int airy_event_loop_add_fd(airy_event_loop_t *loop, int fd, uint32_t events,
                           airy_event_callback_t cb, void *user_data)
{
    return add_fd_internal(loop, fd, events, cb, user_data, false);
}

int airy_event_loop_add_fd_lt(airy_event_loop_t *loop, int fd, uint32_t events,
                              airy_event_callback_t cb, void *user_data)
{
    return add_fd_internal(loop, fd, events, cb, user_data, true);
}

int airy_event_loop_mod_fd(airy_event_loop_t *loop, int fd, uint32_t events)
{
    if (!loop || fd < 0 || fd >= loop->max_events)
        return AIRY_ERR_INVALID_PARAM;

    struct epoll_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;

    if (events & AIRY_EVENT_TYPE_READ)
        ev.events |= EPOLLIN;
    if (events & AIRY_EVENT_TYPE_WRITE)
        ev.events |= EPOLLOUT;

    if (fd < loop->max_events && !loop->fd_entries[fd].level_triggered) {
        ev.events |= EPOLLET;
    }

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        AIRY_ERROR(AIRY_ERR_IO, "epoll_ctl MOD failed");
    }

    loop->fd_entries[fd].events = events;
    return 0;
}

void airy_event_loop_remove_fd(airy_event_loop_t *loop, int fd)
{
    if (!loop || fd < 0 || fd >= loop->max_events)
        return;
    epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    __builtin_memset(&loop->fd_entries[fd], 0, sizeof(fd_entry_t));
}

int airy_event_loop_run(airy_event_loop_t *loop)
{
    if (!loop)
        return AIRY_ERR_INVALID_PARAM;

    loop->running = true;
    loop->stop_requested = false;
    AIRY_LOG_INFO("Event loop started (max_events=%d)", loop->max_events);

    while (!loop->stop_requested) {
        int timeout_ms = 100;
        int nfds = epoll_wait(loop->epoll_fd, loop->epoll_events, loop->max_events, timeout_ms);

        airy_timer_process(&loop->timers);

        for (int i = 0; i < nfds; i++) {
            int fd = loop->epoll_events[i].data.fd;
            uint32_t revents = loop->epoll_events[i].events;

            if (loop->wakeup_fd >= 0 && fd == loop->wakeup_fd) {
                uint64_t val;
                while (read(loop->wakeup_fd, &val, sizeof(val)) > 0) {
                }
                continue;
            }

            uint32_t user_events = 0;
            if (revents & (EPOLLIN | EPOLLHUP | EPOLLERR))
                user_events |= AIRY_EVENT_TYPE_READ;
            if (revents & EPOLLOUT)
                user_events |= AIRY_EVENT_TYPE_WRITE;

            if (fd >= 0 && fd < loop->max_events && loop->fd_entries[fd].cb) {
                loop->fd_entries[fd].cb(fd, user_events, loop->fd_entries[fd].user_data);
            } else if (fd >= loop->max_events) {
                AIRY_LOG_DEBUG("epoll event for fd=%d >= max_events=%d, dropping", fd, loop->max_events);
            }
        }
    }

    loop->running = false;
    AIRY_LOG_INFO("Event loop stopped");
    return 0;
}

/**
 * @brief Async-safe stop of the event loop (safe in signal handlers).
 *
 * Only sets stop_requested and writes the wakeup eventfd; no logging, no
 * locks, satisfying async-signal-safe (write() is an async-safe syscall).
 */
void airy_event_loop_stop_async(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    loop->stop_requested = true;
    if (loop->wakeup_fd >= 0) {
        uint64_t val = 1;
        /* eventfd wakeup is best-effort: a failed write leaves the loop
         * waiting until the next poll tick, never deadlocks. */
        ssize_t n = write(loop->wakeup_fd, &val, sizeof(val));
        (void)n;
    }
}

int airy_event_loop_wakeup(airy_event_loop_t *loop)
{
    if (!loop || loop->wakeup_fd < 0)
        return AIRY_ERR_INVALID_PARAM;
    uint64_t val = 1;
    if (write(loop->wakeup_fd, &val, sizeof(val)) < 0) {
        AIRY_ERROR(AIRY_ERR_IO, "wakeup write failed");
    }
    return 0;
}

int airy_event_loop_get_fd_count(airy_event_loop_t *loop)
{
    if (!loop)
        return 0;
    int count = 0;
    for (int i = 0; i < loop->max_events; i++) {
        if (loop->fd_entries[i].fd > 0)
            count++;
    }
    return count;
}

#else

/* 本 TU 在非 Linux 平台为空：后端选择见 airy_event_loop_win.c /
 * airy_event_loop_kqueue.c。保留一个外部引用锚点防止 ISO 对空翻译单元
 * 的诊断。 */
extern int airy_event_loop_epoll_not_selected;

#endif /* !defined(_WIN32) && defined(__linux__) */
