// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file hall_writer.c
 * @brief Daemon-side hall event recording (write side).
 *
 * Thin delegation to the single-source-of-truth hall event writer
 * (commons/utils/hall/hall_event.c): on-disk root, file naming
 * (tenant.task.category.ts.seq:04u.json), gseq resumption from the disk
 * maximum, decision-chain prev_file linkage, write-role policy, atomic
 * write and the write-then-read assertion all live in that one
 * authoritative mechanism. This file keeps only the daemon-facing entry
 * point so the daemon call sites (sched_d / tool_d / agent_d / daemon_dep)
 * stay unchanged. The delegation also inherits the SSoT path-traversal
 * validation (hall_comp_valid), which the former local copy lacked.
 */

// @owner: team-B
#include "hall_writer.h"

#include "hall_event.h"

int daemon_hall_write(const char *task_id, const char *category, const char *node_id,
                      const char *content_json)
{
    return hall_evt_write(task_id, category, node_id, content_json);
}
