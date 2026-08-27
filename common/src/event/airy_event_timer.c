// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file airy_event_timer.c
 * @brief Platform-independent timer management for the event loop.
 *
 * Extracted from airy_event_loop.c to eliminate timer-code duplication
 * across the three platform backends (IOCP / epoll / kqueue).
 */

#include "airy_event_timer.h"

#include "daemon_errors.h"
#include "airy_memory.h"

#include <string.h>

#ifdef _WIN32
#include "daemon_platform_ext.h"
#else
#include <time.h>
#endif

/* ------------------------------------------------------------------ */
/* Platform monotonic clock                                            */
/* ------------------------------------------------------------------ */

static uint64_t get_time_ms(void)
{
#ifdef _WIN32
    return airy_time_ms();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void airy_timer_init(airy_timer_state_t *ts)
{
    if (!ts)
        return;
    AIRY_MEMSET(ts, 0, sizeof(*ts));
    ts->next_timer_id = 1;
    for (int i = 0; i < AIRY_TIMER_MAX_TIMERS; i++)
        ts->timers[i].active = false;
}

/* ------------------------------------------------------------------ */
/* Public API delegates                                                */
/* ------------------------------------------------------------------ */

uint64_t airy_timer_add(airy_timer_state_t *ts, uint64_t interval_ms,
                         airy_timer_callback_t cb, void *user_data)
{
    if (!ts || !cb)
        return 0;

    for (int i = 0; i < AIRY_TIMER_MAX_TIMERS; i++) {
        if (!ts->timers[i].active) {
            ts->timers[i].id = ts->next_timer_id++;
            ts->timers[i].interval_ms = interval_ms;
            ts->timers[i].next_fire_ms = ts->current_time_ms + interval_ms;
            ts->timers[i].cb = cb;
            ts->timers[i].user_data = user_data;
            ts->timers[i].active = true;
            return ts->timers[i].id;
        }
    }
    return 0;
}

int airy_timer_cancel(airy_timer_state_t *ts, uint64_t timer_id)
{
    if (!ts || timer_id == 0)
        return AIRY_ERR_INVALID_PARAM;

    for (int i = 0; i < AIRY_TIMER_MAX_TIMERS; i++) {
        if (ts->timers[i].active && ts->timers[i].id == timer_id) {
            ts->timers[i].active = false;
            return 0;
        }
    }
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "timer not found");
}

/* ------------------------------------------------------------------ */
/* Run-loop helper                                                     */
/* ------------------------------------------------------------------ */

void airy_timer_process(airy_timer_state_t *ts)
{
    if (!ts)
        return;

    ts->current_time_ms = get_time_ms();

    for (int i = 0; i < AIRY_TIMER_MAX_TIMERS; i++) {
        if (ts->timers[i].active && ts->current_time_ms >= ts->timers[i].next_fire_ms) {
            ts->timers[i].cb(NULL, ts->timers[i].id, ts->timers[i].user_data);
            if (ts->timers[i].active)
                ts->timers[i].next_fire_ms = ts->current_time_ms + ts->timers[i].interval_ms;
        }
    }
}
