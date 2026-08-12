/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

#ifndef AIRY_RT_EVENT_LOOP_H
#define AIRY_RT_EVENT_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIRY_EVENT_LOOP_MAX_EVENTS 1024
#define AIRY_EVENT_LOOP_MAX_TIMERS 64

typedef enum {
    AIRY_EVENT_TYPE_READ = 1,
    AIRY_EVENT_TYPE_WRITE = 2,
    AIRY_EVENT_TYPE_TIMER = 4,
    AIRY_EVENT_TYPE_SIGNAL = 8
} airy_event_type_t;

typedef struct airy_event_loop airy_event_loop_t;

typedef int (*airy_event_callback_t)(int fd, uint32_t events, void *user_data);

typedef void (*airy_timer_callback_t)(airy_event_loop_t *loop, uint64_t timer_id, void *user_data);

airy_event_loop_t *airy_event_loop_create(int max_events);

void airy_event_loop_destroy(airy_event_loop_t *loop);

int airy_event_loop_add_fd(airy_event_loop_t *loop, int fd, uint32_t events,
                           airy_event_callback_t cb, void *user_data);

int airy_event_loop_add_fd_lt(airy_event_loop_t *loop, int fd, uint32_t events,
                              airy_event_callback_t cb, void *user_data);

int airy_event_loop_mod_fd(airy_event_loop_t *loop, int fd, uint32_t events);

void airy_event_loop_remove_fd(airy_event_loop_t *loop, int fd);

uint64_t airy_event_loop_add_timer(airy_event_loop_t *loop, uint64_t interval_ms,
                                   airy_timer_callback_t cb, void *user_data);

int airy_event_loop_cancel_timer(airy_event_loop_t *loop, uint64_t timer_id);

int airy_event_loop_run(airy_event_loop_t *loop);

void airy_event_loop_stop(airy_event_loop_t *loop);

/**
 * @brief Async-safe stop of the event loop (safe in signal handlers).
 *
 * Only performs an atomic flag set plus eventfd/event wakeup; calls no log
 * or lock primitives, keeping it async-signal-safe and avoiding deadlock
 * between a signal handler and the logging lock.
 */
void airy_event_loop_stop_async(airy_event_loop_t *loop);

int airy_event_loop_get_fd_count(airy_event_loop_t *loop);

int airy_event_loop_wakeup(airy_event_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif
