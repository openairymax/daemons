// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_event_loop.h"

#include "daemon_errors.h"
#include "airy_memory.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "error.h"

#ifdef _WIN32
#include "daemon_platform_ext.h"
#include "svc_logger.h"

typedef struct {
    uint64_t id;
    uint64_t interval_ms;
    uint64_t next_fire_ms;
    airy_timer_callback_t cb;
    void *user_data;
    bool active;
} timer_entry_t;

typedef struct {
    SOCKET fd;
    WSAEVENT wsa_event;
    uint32_t events;
    airy_event_callback_t cb;
    void *user_data;
    bool level_triggered;
    bool in_use;
} fd_entry_t;

struct airy_event_loop {
    fd_entry_t *fd_entries;
    int fd_count;
    int fd_capacity;
    int max_events;
    HANDLE wakeup_event;
    timer_entry_t timers[AIRY_EVENT_LOOP_MAX_TIMERS];
    uint64_t next_timer_id;
    uint64_t current_time_ms;
    volatile bool running;
    volatile bool stop_requested;
};

static uint64_t get_time_ms(void)
{
    return airy_time_ms();
}

static int find_fd_entry(airy_event_loop_t *loop, SOCKET sock)
{
    for (int i = 0; i < loop->fd_capacity; i++) {
        if (loop->fd_entries[i].in_use && loop->fd_entries[i].fd == sock)
            return i;
    }
    return AIRY_ERR_NOT_FOUND;
}

static long events_to_wsa(uint32_t events)
{
    long wsa = 0;
    if (events & AIRY_EVENT_TYPE_READ)
        wsa |= FD_READ | FD_ACCEPT | FD_CLOSE;
    if (events & AIRY_EVENT_TYPE_WRITE)
        wsa |= FD_WRITE | FD_CONNECT;
    return wsa;
}

static uint32_t wsa_to_events(long wsa_events)
{
    uint32_t ev = 0;
    if (wsa_events & (FD_READ | FD_ACCEPT | FD_CLOSE))
        ev |= AIRY_EVENT_TYPE_READ;
    if (wsa_events & (FD_WRITE | FD_CONNECT))
        ev |= AIRY_EVENT_TYPE_WRITE;
    return ev;
}

static int add_fd_internal(airy_event_loop_t *loop, int fd, uint32_t events,
                           airy_event_callback_t cb, void *user_data, bool level_triggered)
{
    if (!loop || fd < 0 || !cb)
        return AIRY_ERR_INVALID_PARAM;

    SOCKET sock = (SOCKET)fd;
    int idx = find_fd_entry(loop, sock);

    if (idx >= 0) {
        loop->fd_entries[idx].events = events;
        loop->fd_entries[idx].cb = cb;
        loop->fd_entries[idx].user_data = user_data;
        loop->fd_entries[idx].level_triggered = level_triggered;

        long wsa_events = events_to_wsa(events);
        if (WSAEventSelect(sock, loop->fd_entries[idx].wsa_event, wsa_events) != 0) {
            AIRY_LOG_DEBUG("WSAEventSelect MOD failed for fd=%d: %d", fd, WSAGetLastError());
            return AIRY_ERR_IO;
        }
        return 0;
    }

    if (loop->fd_count >= loop->fd_capacity) {
        AIRY_LOG_DEBUG("fd capacity reached (%d), cannot add fd=%d", loop->fd_capacity, fd);
        return AIRY_ERR_OVERFLOW;
    }

    int free_idx = -1;
    for (int i = 0; i < loop->fd_capacity; i++) {
        if (!loop->fd_entries[i].in_use) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        AIRY_ERROR(AIRY_ERR_NOT_FOUND, "no free fd slot available");
    }

    WSAEVENT wsa_event = WSACreateEvent();
    if (wsa_event == WSA_INVALID_EVENT) {
        AIRY_LOG_DEBUG("WSACreateEvent failed for fd=%d: %d", fd, WSAGetLastError());
        return AIRY_ERR_IO;
    }

    long wsa_events = events_to_wsa(events);
    if (WSAEventSelect(sock, wsa_event, wsa_events) != 0) {
        AIRY_LOG_DEBUG("WSAEventSelect ADD failed for fd=%d: %d", fd, WSAGetLastError());
        WSACloseEvent(wsa_event);
        return AIRY_ERR_IO;
    }

    loop->fd_entries[free_idx].fd = sock;
    loop->fd_entries[free_idx].wsa_event = wsa_event;
    loop->fd_entries[free_idx].events = events;
    loop->fd_entries[free_idx].cb = cb;
    loop->fd_entries[free_idx].user_data = user_data;
    loop->fd_entries[free_idx].level_triggered = level_triggered;
    loop->fd_entries[free_idx].in_use = true;
    loop->fd_count++;

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

    loop->max_events = max_events;
    loop->fd_capacity = max_events;
    loop->fd_entries = (fd_entry_t *)AIRY_CALLOC((size_t)max_events, sizeof(fd_entry_t));
    if (!loop->fd_entries) {
        AIRY_FREE(loop);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    loop->wakeup_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!loop->wakeup_event) {
        AIRY_FREE(loop->fd_entries);
        AIRY_FREE(loop);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    loop->next_timer_id = 1;
    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++)
        loop->timers[i].active = false;

    AIRY_LOG_DEBUG("Event loop created (max_events=%d)", max_events);
    return loop;
}

void airy_event_loop_destroy(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    for (int i = 0; i < loop->fd_capacity; i++) {
        if (loop->fd_entries[i].in_use) {
            WSAEventSelect(loop->fd_entries[i].fd, loop->fd_entries[i].wsa_event, 0);
            WSACloseEvent(loop->fd_entries[i].wsa_event);
        }
    }
    if (loop->wakeup_event)
        CloseHandle(loop->wakeup_event);
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
    if (!loop || fd < 0)
        return AIRY_ERR_INVALID_PARAM;
    SOCKET sock = (SOCKET)fd;
    int idx = find_fd_entry(loop, sock);
    if (idx < 0) {
        AIRY_ERROR(AIRY_ERR_NOT_FOUND, "fd not found for modify");
    }

    long wsa_events = events_to_wsa(events);
    if (WSAEventSelect(sock, loop->fd_entries[idx].wsa_event, wsa_events) != 0) {
        AIRY_LOG_DEBUG("WSAEventSelect MOD failed for fd=%d: %d", fd, WSAGetLastError());
        return AIRY_ERR_IO;
    }
    loop->fd_entries[idx].events = events;
    return 0;
}

void airy_event_loop_remove_fd(airy_event_loop_t *loop, int fd)
{
    if (!loop || fd < 0)
        return;
    SOCKET sock = (SOCKET)fd;
    int idx = find_fd_entry(loop, sock);
    if (idx < 0)
        return;

    WSAEventSelect(sock, loop->fd_entries[idx].wsa_event, 0);
    WSACloseEvent(loop->fd_entries[idx].wsa_event);
    __builtin_memset(&loop->fd_entries[idx], 0, sizeof(fd_entry_t));
    loop->fd_count--;
}

uint64_t airy_event_loop_add_timer(airy_event_loop_t *loop, uint64_t interval_ms,
                                   airy_timer_callback_t cb, void *user_data)
{
    if (!loop || !cb)
        return 0;
    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (!loop->timers[i].active) {
            loop->timers[i].id = loop->next_timer_id++;
            loop->timers[i].interval_ms = interval_ms;
            loop->timers[i].next_fire_ms = loop->current_time_ms + interval_ms;
            loop->timers[i].cb = cb;
            loop->timers[i].user_data = user_data;
            loop->timers[i].active = true;
            return loop->timers[i].id;
        }
    }
    return 0;
}

int airy_event_loop_cancel_timer(airy_event_loop_t *loop, uint64_t timer_id)
{
    if (!loop || timer_id == 0)
        return AIRY_ERR_INVALID_PARAM;
    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->timers[i].id == timer_id) {
            loop->timers[i].active = false;
            return 0;
        }
    }
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "timer not found");
}

static void process_timers(airy_event_loop_t *loop)
{
    loop->current_time_ms = get_time_ms();
    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->current_time_ms >= loop->timers[i].next_fire_ms) {
            loop->timers[i].cb(loop, loop->timers[i].id, loop->timers[i].user_data);
            if (loop->timers[i].active)
                loop->timers[i].next_fire_ms = loop->current_time_ms + loop->timers[i].interval_ms;
        }
    }
}

int airy_event_loop_run(airy_event_loop_t *loop)
{
    if (!loop)
        return AIRY_ERR_INVALID_PARAM;

    loop->running = true;
    loop->stop_requested = false;
    AIRY_LOG_INFO("Event loop started (max_events=%d)", loop->max_events);

    while (!loop->stop_requested) {
        WSAEVENT wait_events[WSA_MAXIMUM_WAIT_EVENTS];
        int fd_map[WSA_MAXIMUM_WAIT_EVENTS];
        int event_count = 0;

        wait_events[event_count] = (WSAEVENT)loop->wakeup_event;
        fd_map[event_count] = -1;
        event_count++;

        for (int i = 0; i < loop->fd_capacity && event_count < WSA_MAXIMUM_WAIT_EVENTS; i++) {
            if (loop->fd_entries[i].in_use) {
                wait_events[event_count] = loop->fd_entries[i].wsa_event;
                fd_map[event_count] = i;
                event_count++;
            }
        }

        DWORD wait_result =
            WSAWaitForMultipleEvents((DWORD)event_count, wait_events, FALSE, 100, FALSE);

        process_timers(loop);

        if (wait_result == WSA_WAIT_FAILED) {
            AIRY_LOG_DEBUG("WSAWaitForMultipleEvents failed: %d", WSAGetLastError());
            continue;
        }
        if (wait_result == WSA_WAIT_TIMEOUT)
            continue;

        int start_idx = (int)(wait_result - WSA_WAIT_EVENT_0);
        for (int ei = start_idx; ei < event_count; ei++) {
            if (WaitForSingleObject(wait_events[ei], 0) != WAIT_OBJECT_0)
                continue;

            if (fd_map[ei] < 0) {
                ResetEvent(loop->wakeup_event);
                continue;
            }

            int fi = fd_map[ei];
            if (!loop->fd_entries[fi].in_use)
                continue;

            WSANETWORKEVENTS net_events;
            if (WSAEnumNetworkEvents(loop->fd_entries[fi].fd, loop->fd_entries[fi].wsa_event,
                                     &net_events) != 0) {
                AIRY_LOG_DEBUG("WSAEnumNetworkEvents failed for fd=%d: %d", (int)loop->fd_entries[fi].fd,
                          WSAGetLastError());
                continue;
            }

            uint32_t user_events = wsa_to_events(net_events.lNetworkEvents);
            if (net_events.lNetworkEvents & FD_CLOSE)
                user_events |= AIRY_EVENT_TYPE_READ;

            if (user_events && loop->fd_entries[fi].cb) {
                loop->fd_entries[fi].cb((int)loop->fd_entries[fi].fd, user_events,
                                        loop->fd_entries[fi].user_data);
            }
        }
    }

    loop->running = false;
    AIRY_LOG_INFO("Event loop stopped");
    return 0;
}

void airy_event_loop_stop(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    airy_event_loop_stop_async(loop);
    AIRY_LOG_DEBUG("Event loop stop requested");
}

/**
 * @brief Async-safe stop of the event loop (safe in signal handlers).
 *
 * Only sets stop_requested and SetEvent-wakes; no logging, no locks.
 */
void airy_event_loop_stop_async(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    loop->stop_requested = true;
    if (loop->wakeup_event)
        SetEvent(loop->wakeup_event);
}

int airy_event_loop_wakeup(airy_event_loop_t *loop)
{
    if (!loop || !loop->wakeup_event)
        return AIRY_ERR_INVALID_PARAM;
    if (!SetEvent(loop->wakeup_event)) {
        AIRY_ERROR(AIRY_ERR_IO, "wakeup SetEvent failed");
    }
    return 0;
}

int airy_event_loop_get_fd_count(airy_event_loop_t *loop)
{
    if (!loop)
        return 0;
    return loop->fd_count;
}

#elif defined(__linux__)

#include <sys/epoll.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#include "svc_logger.h"

typedef struct {
    uint64_t id;
    uint64_t interval_ms;
    uint64_t next_fire_ms;
    airy_timer_callback_t cb;
    void *user_data;
    bool active;
} timer_entry_t;

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
    timer_entry_t timers[AIRY_EVENT_LOOP_MAX_TIMERS];
    uint64_t next_timer_id;
    uint64_t current_time_ms;
    volatile bool running;
    volatile bool stop_requested;
};

static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
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

    loop->next_timer_id = 1;
    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        loop->timers[i].active = false;
    }

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

uint64_t airy_event_loop_add_timer(airy_event_loop_t *loop, uint64_t interval_ms,
                                   airy_timer_callback_t cb, void *user_data)
{
    if (!loop || !cb)
        return 0;

    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (!loop->timers[i].active) {
            loop->timers[i].id = loop->next_timer_id++;
            loop->timers[i].interval_ms = interval_ms;
            loop->timers[i].next_fire_ms = loop->current_time_ms + interval_ms;
            loop->timers[i].cb = cb;
            loop->timers[i].user_data = user_data;
            loop->timers[i].active = true;
            return loop->timers[i].id;
        }
    }
    return 0;
}

int airy_event_loop_cancel_timer(airy_event_loop_t *loop, uint64_t timer_id)
{
    if (!loop || timer_id == 0)
        return AIRY_ERR_INVALID_PARAM;

    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->timers[i].id == timer_id) {
            loop->timers[i].active = false;
            return 0;
        }
    }
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "timer not found");
}

static void process_timers(airy_event_loop_t *loop)
{
    loop->current_time_ms = get_time_ms();

    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->current_time_ms >= loop->timers[i].next_fire_ms) {
            loop->timers[i].cb(loop, loop->timers[i].id, loop->timers[i].user_data);
            if (loop->timers[i].active) {
                loop->timers[i].next_fire_ms = loop->current_time_ms + loop->timers[i].interval_ms;
            }
        }
    }
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

        process_timers(loop);

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

void airy_event_loop_stop(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    airy_event_loop_stop_async(loop);
    AIRY_LOG_DEBUG("Event loop stop requested");
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
        (void)write(loop->wakeup_fd, &val, sizeof(val));
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

/* macOS / BSD kqueue 后端（Airymax 0.1.2 新增，替代原 #error 的
 * "macOS 支持未规划" 设计限制）。设计对齐上方 epoll 后端：
 *   - fd 直接作为 kevent.ident 与 fd_entries 数组索引（fd < max_events）；
 *   - 定时器沿用 process_timers 轮询模式（kevent 100ms timeout），不引入
 *     EVFILT_TIMER，保证与 Linux/Windows 后端行为完全一致；
 *   - wakeup 用 EVFILT_USER（macOS 10.6+）：stop_async 内仅 kevent 一个
 *     syscall，保持 async-signal-safe（对齐 epoll 后端 eventfd 语义）。
 *   - kqueue 天然 level-triggered；level_triggered=false 时加 EV_CLEAR
 *     获得 edge-triggered 语义（对齐 EPOLLET）。 */

#include <sys/event.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "svc_logger.h"

/* EVFILT_USER 的独立 ident：选一个远离真实 fd 的值，避免与 fd_entries
 * 索引（[0, max_events)）冲突。 */
#define AIRY_KQ_WAKEUP_IDENT 0x40000001u

typedef struct {
    uint64_t id;
    uint64_t interval_ms;
    uint64_t next_fire_ms;
    airy_timer_callback_t cb;
    void *user_data;
    bool active;
} timer_entry_t;

typedef struct {
    int fd;
    uint32_t events;
    airy_event_callback_t cb;
    void *user_data;
    bool level_triggered;
} fd_entry_t;

struct airy_event_loop {
    int kq;
    int max_events;
    struct kevent *kq_events;
    fd_entry_t *fd_entries;
    timer_entry_t timers[AIRY_EVENT_LOOP_MAX_TIMERS];
    uint64_t next_timer_id;
    uint64_t current_time_ms;
    volatile bool running;
    volatile bool stop_requested;
};

static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
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

    struct kevent kev[2];
    int n = 0;
    if (events & AIRY_EVENT_TYPE_READ) {
        EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    }
    if (events & AIRY_EVENT_TYPE_WRITE) {
        EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    }
    if (!level_triggered) {
        /* kqueue 默认 level-triggered；非 level 模式追加 EV_CLEAR 获得
         * edge-triggered 语义（对齐 EPOLLET）。 */
        for (int i = 0; i < n; i++)
            kev[i].flags |= EV_CLEAR;
    }

    if (n > 0) {
        if (kevent(loop->kq, kev, n, NULL, 0, NULL) < 0) {
            AIRY_LOG_DEBUG("kevent ADD failed for fd=%d: %s", fd, strerror(errno));
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

    loop->kq = kqueue();
    if (loop->kq < 0) {
        AIRY_FREE(loop);
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    loop->max_events = max_events;
    loop->kq_events = (struct kevent *)AIRY_CALLOC((size_t)max_events, sizeof(struct kevent));
    loop->fd_entries = (fd_entry_t *)AIRY_CALLOC((size_t)max_events, sizeof(fd_entry_t));

    if (!loop->kq_events || !loop->fd_entries) {
        close(loop->kq);
        AIRY_FREE(loop->kq_events);
        AIRY_FREE(loop->fd_entries);
        AIRY_FREE(loop);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* wakeup 事件：EVFILT_USER 用户事件过滤器（macOS 10.6+）。 */
    struct kevent wake_kev;
    EV_SET(&wake_kev, AIRY_KQ_WAKEUP_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_FFNOP, 0, NULL);
    if (kevent(loop->kq, &wake_kev, 1, NULL, 0, NULL) < 0) {
        close(loop->kq);
        AIRY_FREE(loop->kq_events);
        AIRY_FREE(loop->fd_entries);
        AIRY_FREE(loop);
        AIRY_ERROR_NULL(AIRY_ERR_IO, "io error");
    }

    loop->next_timer_id = 1;
    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        loop->timers[i].active = false;
    }

    AIRY_LOG_DEBUG("Event loop created (kq=%d, max_events=%d)", loop->kq, max_events);
    return loop;
}

void airy_event_loop_destroy(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    close(loop->kq);
    AIRY_FREE(loop->kq_events);
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

    struct kevent kev[2];
    int n = 0;
    if (events & AIRY_EVENT_TYPE_READ) {
        EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    }
    if (events & AIRY_EVENT_TYPE_WRITE) {
        EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    }
    if (n > 0 && !loop->fd_entries[fd].level_triggered) {
        for (int i = 0; i < n; i++)
            kev[i].flags |= EV_CLEAR;
    }

    if (n > 0) {
        if (kevent(loop->kq, kev, n, NULL, 0, NULL) < 0) {
            AIRY_ERROR(AIRY_ERR_IO, "kevent MOD failed");
        }
    }

    loop->fd_entries[fd].events = events;
    return 0;
}

void airy_event_loop_remove_fd(airy_event_loop_t *loop, int fd)
{
    if (!loop || fd < 0 || fd >= loop->max_events)
        return;

    struct kevent kev;
    EV_SET(&kev, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(loop->kq, &kev, 1, NULL, 0, NULL);
    EV_SET(&kev, (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(loop->kq, &kev, 1, NULL, 0, NULL);

    __builtin_memset(&loop->fd_entries[fd], 0, sizeof(fd_entry_t));
}

uint64_t airy_event_loop_add_timer(airy_event_loop_t *loop, uint64_t interval_ms,
                                   airy_timer_callback_t cb, void *user_data)
{
    if (!loop || !cb)
        return 0;

    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (!loop->timers[i].active) {
            loop->timers[i].id = loop->next_timer_id++;
            loop->timers[i].interval_ms = interval_ms;
            loop->timers[i].next_fire_ms = loop->current_time_ms + interval_ms;
            loop->timers[i].cb = cb;
            loop->timers[i].user_data = user_data;
            loop->timers[i].active = true;
            return loop->timers[i].id;
        }
    }
    return 0;
}

int airy_event_loop_cancel_timer(airy_event_loop_t *loop, uint64_t timer_id)
{
    if (!loop || timer_id == 0)
        return AIRY_ERR_INVALID_PARAM;

    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->timers[i].id == timer_id) {
            loop->timers[i].active = false;
            return 0;
        }
    }
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "timer not found");
}

static void process_timers(airy_event_loop_t *loop)
{
    loop->current_time_ms = get_time_ms();

    for (int i = 0; i < AIRY_EVENT_LOOP_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->current_time_ms >= loop->timers[i].next_fire_ms) {
            loop->timers[i].cb(loop, loop->timers[i].id, loop->timers[i].user_data);
            if (loop->timers[i].active) {
                loop->timers[i].next_fire_ms = loop->current_time_ms + loop->timers[i].interval_ms;
            }
        }
    }
}

int airy_event_loop_run(airy_event_loop_t *loop)
{
    if (!loop)
        return AIRY_ERR_INVALID_PARAM;

    loop->running = true;
    loop->stop_requested = false;
    AIRY_LOG_INFO("Event loop started (max_events=%d)", loop->max_events);

    while (!loop->stop_requested) {
        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        int nev = kevent(loop->kq, NULL, 0, loop->kq_events, loop->max_events, &timeout);

        process_timers(loop);

        if (nev < 0) {
            if (errno == EINTR)
                continue;
            AIRY_ERROR(AIRY_ERR_IO, "kevent wait failed");
        }

        for (int i = 0; i < nev; i++) {
            struct kevent *kev = &loop->kq_events[i];
            uintptr_t ident = (uintptr_t)kev->ident;

            if (ident == AIRY_KQ_WAKEUP_IDENT)
                continue; /* wakeup 事件：仅用于打断 kevent 阻塞 */

            int fd = (int)ident;
            uint32_t user_events = 0;
            if (kev->filter == EVFILT_READ)
                user_events |= AIRY_EVENT_TYPE_READ;
            if (kev->filter == EVFILT_WRITE)
                user_events |= AIRY_EVENT_TYPE_WRITE;

            if (fd >= 0 && fd < loop->max_events && loop->fd_entries[fd].cb) {
                loop->fd_entries[fd].cb(fd, user_events, loop->fd_entries[fd].user_data);
            } else if (fd >= loop->max_events) {
                AIRY_LOG_DEBUG("kevent event for fd=%d >= max_events=%d, dropping", fd, loop->max_events);
            }
        }
    }

    loop->running = false;
    AIRY_LOG_INFO("Event loop stopped");
    return 0;
}

void airy_event_loop_stop(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    airy_event_loop_stop_async(loop);
    AIRY_LOG_DEBUG("Event loop stop requested");
}

/**
 * @brief Async-safe stop of the event loop (safe in signal handlers).
 *
 * Sets stop_requested and triggers the EVFILT_USER wakeup event; kevent()
 * is an async-safe syscall, no logging or locks involved.
 */
void airy_event_loop_stop_async(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    loop->stop_requested = true;
    struct kevent kev;
    EV_SET(&kev, AIRY_KQ_WAKEUP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    (void)kevent(loop->kq, &kev, 1, NULL, 0, NULL);
}

int airy_event_loop_wakeup(airy_event_loop_t *loop)
{
    if (!loop)
        return AIRY_ERR_INVALID_PARAM;
    struct kevent kev;
    EV_SET(&kev, AIRY_KQ_WAKEUP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    if (kevent(loop->kq, &kev, 1, NULL, 0, NULL) < 0) {
        AIRY_ERROR(AIRY_ERR_IO, "wakeup kevent failed");
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

#endif
