// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file airy_event_timer.h
 * @brief Internal header: timer management for the event loop.
 *
 * Provides a self-contained timer subsystem shared by all platform
 * backends (epoll / kqueue / IOCP).  The backend creates an
 * airy_timer_state_t alongside the event loop and calls
 * airy_timer_process() after each poll cycle.
 *
 * NOT part of the public API.
 */

#ifndef AIRY_EVENT_TIMER_INTERNAL_H
#define AIRY_EVENT_TIMER_INTERNAL_H

#include "airy_event_loop.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIRY_TIMER_MAX_TIMERS AIRY_EVENT_LOOP_MAX_TIMERS

typedef struct {
    uint64_t id;
    uint64_t interval_ms;
    uint64_t next_fire_ms;
    airy_timer_callback_t cb;
    void *user_data;
    bool active;
} airy_timer_entry_t;

/** Self-contained timer state (no platform dependencies). */
typedef struct {
    airy_timer_entry_t timers[AIRY_TIMER_MAX_TIMERS];
    uint64_t next_timer_id;
    uint64_t current_time_ms;
} airy_timer_state_t;

/* --- lifecycle ---------------------------------------------------------- */

void airy_timer_init(airy_timer_state_t *ts);

/* --- public API delegates (called from airy_event_loop.c) --------------- */

uint64_t airy_timer_add(airy_timer_state_t *ts, uint64_t interval_ms,
                         airy_timer_callback_t cb, void *user_data);

int airy_timer_cancel(airy_timer_state_t *ts, uint64_t timer_id);

/* --- run-loop helper ---------------------------------------------------- */

/**
 * @brief Refresh current_time_ms and fire all due timers.
 *
 * The backend calls this after each poll/kevent/WSAWait iteration.
 * The get_time_ms callback supplies the platform monotonic clock.
 */
void airy_timer_process(airy_timer_state_t *ts);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_EVENT_TIMER_INTERNAL_H */
