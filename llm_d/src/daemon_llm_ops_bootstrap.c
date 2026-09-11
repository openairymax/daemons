// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_llm_ops_bootstrap.c
 * @brief LLM service ops-table bootstrap implementation.
 */

#include "daemon_llm_ops_bootstrap.h"

#include "airy_llm_ops.h"

#include "llm_service.h"
#include "svc_logger.h"

static int g_llm_ops_initialized = 0;

/*
 * ARC-04: airy_llm_service-backed implementation of the atoms LLM ops
 * table. The atoms layer (cognition engine, orchestrator, language
 * gateway) dispatches through this table without linking against daemons
 * symbols (ARC-02); the daemon layer owns the concrete implementation and
 * injects it here. Cleared on cleanup, after which atoms callers degrade
 * gracefully (BAN-319).
 *
 * The table storage (are_ops_set/get_llm) lives in the zero-dependency
 * small library airy_llm_ops, not in airy_llm_service itself, so that
 * atoms consumers resolve the accessor without an atoms->daemons edge;
 * the daemon links that small library PRIVATE (see llm_d/CMakeLists.txt).
 */
static const airy_llm_ops_t g_daemon_llm_ops = {
    /* LLM service (daemons: llm_service.h) */
    .service_complete = llm_service_complete,
    .service_complete_stream = llm_service_complete_stream,
    .response_free = llm_response_free,
};

airy_err_t daemon_llm_ops_init(const char *daemon_name)
{
    if (!daemon_name) {
        SVC_LOG_ERROR("daemon_llm_ops_init: NULL daemon_name");
        return AIRY_EINVAL;
    }

    if (g_llm_ops_initialized) {
        SVC_LOG_DEBUG("daemon_llm_ops_init: LLM ops already injected (daemon=%s)", daemon_name);
        return AIRY_SUCCESS;
    }

    are_ops_set_llm(&g_daemon_llm_ops);
    g_llm_ops_initialized = 1;
    SVC_LOG_INFO("daemon_llm_ops_init: LLM service ops injected for '%s'", daemon_name);
    return AIRY_SUCCESS;
}

void daemon_llm_ops_cleanup(void)
{
    if (!g_llm_ops_initialized)
        return;

    are_ops_set_llm(NULL);
    g_llm_ops_initialized = 0;
    SVC_LOG_INFO("daemon_llm_ops_cleanup: LLM service ops detached");
}
