/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file hall_writer.h
 * @brief Daemon-side hall event writer (write side of the event flow).
 *
 * Lets any daemon process (sched_d / tool_d / agent_d / ...) record
 * execution-chain events into the single-source-of-truth hall event store,
 * aligned byte-for-byte with the authoritative writers:
 *   - runtime writer : atoms/coreloopthree/src/dispatch/hall_store.c
 *   - gateway writer : gateway/src/gateway/gateway_hall_store.c
 * Same root (airy_data_dir()/agentrt/hall), same file naming
 * ({tenant}.{task}.{category}.{ts_utc}.{seq:04u}.json), same event body
 * header/access layout. Each daemon is a writer process of its own: gseq
 * is per-process (audit only), cross-process order is (ts_utc, seq).
 *
 * prev_file linkage mirrors hall_store.c: each event records the file id of
 * the previous event in the same (task, category) directory (the max-seq
 * file on disk), so the decision chain is reconstructible from the on-disk
 * event flow alone.
 *
 * This writer is best-effort by contract: a failed event write never fails
 * the caller's flow (returns non-zero, caller may ignore).
 */

#ifndef AIRY_RT_DAEMON_HALL_WRITER_H
#define AIRY_RT_DAEMON_HALL_WRITER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record one hall event (task-file model) from a daemon process.
 * @param task_id      task ID used as the event-flow grouping key
 *                     (e.g. scheduler task_id, agent_id, session id)
 * @param category     category name ("blueprint"/"command"/"progress"/
 *                     "result"/"issue"/"verify"/"chain")
 * @param node_id      blueprint node ID, or NULL
 * @param content_json JSON object string for the content section (caller
 *                     guarantees it parses as a JSON object)
 * @return 0 on success, non-zero on failure (never fails the caller's flow)
 *
 * Thread-safe: lazy one-time init + per-write lock, safe to call from
 * worker threads (sched_d worker / tool_d executor).
 */
int daemon_hall_write(const char *task_id, const char *category, const char *node_id,
                      const char *content_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_HALL_WRITER_H */
