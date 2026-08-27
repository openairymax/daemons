// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file airy_event_loop_win.c
 * @brief Event loop Windows backend: WSAEventSelect multiplexing and dispatch.
 *
 * Windows 平台后端（IO 多路复用域）：fd 注册表、WSAEventSelect 订阅与
 * run 主循环内的回调分发。事件循环核心门面见 airy_event_loop.c；定时器
 * 管理见 airy_event_timer.c（本后端仅在每轮等待后推进定时器）。
 *
 * 每个 fd 绑定一个 WSAEVENT；wakeup 复用 CreateEventW 自动复位事件，
 * stop_async 内仅 SetEvent 一个调用，保持 async-signal-safe。
 */

#include "airy_event_loop.h"
#include "airy_event_loop_internal.h"

#include "daemon_errors.h"
#include "airy_memory.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "daemon_platform_ext.h"
#include "svc_logger.h"

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
    airy_timer_state_t timers;
    volatile bool running;
    volatile bool stop_requested;
};

airy_timer_state_t *airy_event_loop_timers(airy_event_loop_t *loop)
{
    return loop ? &loop->timers : NULL;
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

    airy_timer_init(&loop->timers);

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

        airy_timer_process(&loop->timers);

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

#else

/* 本 TU 仅在 Windows 参与编译：后端选择见 airy_event_loop_epoll.c /
 * airy_event_loop_kqueue.c。 */
typedef int airy_event_loop_win_not_selected_t;

#endif /* _WIN32 */
