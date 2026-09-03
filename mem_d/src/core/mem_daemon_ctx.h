// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file mem_daemon_ctx.h
 * @brief Shared daemon context: extern globals, config type, helpers.
 *
 * The mem_d handler modules (mem_handlers, kb_handlers, cache_handlers,
 * ledger_handlers) share access to the global service, cache, ledger
 * instances and the daemon configuration.  This header declares them.
 */

#ifndef MEM_DAEMON_CTX_H
#define MEM_DAEMON_CTX_H

#include "mem_service.h"
#include "cache.h"
#include "ledger.h"
#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Daemon configuration ─────────────────────────────────────────────── */

#define MEM_DEFAULT_MAX_RECORDS 1024
#define MAX_CLIENTS 64

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
    size_t max_records;
} mem_daemon_config_t;

/* ── Global service instances (defined in main.c) ─────────────────────── */

extern mem_service_t  *g_service;
extern mem_cache_t    *g_cache;
extern mem_ledger_t   *g_ledger;
extern mem_daemon_config_t g_config;

/* ── Utility ──────────────────────────────────────────────────────────── */

/** Clamp a signed int64 to [0, max_v]; negative → def, > max_v → max_v. */
static inline uint32_t clamp_u32(int64_t v, uint32_t def, uint32_t max_v)
{
    if (v < 0)
        return def;
    if ((uint64_t)v > (uint64_t)max_v)
        return max_v;
    return (uint32_t)v;
}

#ifdef __cplusplus
}
#endif

#endif /* MEM_DAEMON_CTX_H */
