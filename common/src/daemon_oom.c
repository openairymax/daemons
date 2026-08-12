// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_oom.c
 * @brief Daemon OOM degradation callback registration helper impl.
 *
 * P1.22: standardized OOM degradation callbacks for every daemon.
 */

#include "daemon_oom.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

/** @brief Daemon OOM degradation context. */
typedef struct {
    char daemon_name[64];
    bool drop_cache_on_warning;
    bool reject_requests_on_critical;
    void *user_context;
    degradation_handler_t warning_handler;
    degradation_handler_t critical_handler;
} daemon_oom_ctx_t;

#define DAEMON_OOM_MAX_SLOTS 16

static daemon_oom_ctx_t g_daemons_oom_slots[DAEMON_OOM_MAX_SLOTS];
static int g_daemons_oom_count = 0;

static int daemon_oom_on_warning(degradation_handler_t *handler, watermark_level_t old_level,
                                 watermark_level_t new_level)
{
    daemon_oom_ctx_t *ctx = (daemon_oom_ctx_t *)handler->context;
    (void)old_level;
    (void)new_level;

    if (ctx->drop_cache_on_warning) {
        SVC_LOG_WARN("P1.22: [%s] OOM WARNING — dropping caches", ctx->daemon_name);
    }

    return 0;
}

static int daemon_oom_on_warning_restore(degradation_handler_t *handler,
                                         watermark_level_t old_level, watermark_level_t new_level)
{
    daemon_oom_ctx_t *ctx = (daemon_oom_ctx_t *)handler->context;
    (void)old_level;
    (void)new_level;

    SVC_LOG_INFO("P1.22: [%s] OOM WARNING restored — caches re-enabled", ctx->daemon_name);
    return 0;
}

static int daemon_oom_on_critical(degradation_handler_t *handler, watermark_level_t old_level,
                                  watermark_level_t new_level)
{
    daemon_oom_ctx_t *ctx = (daemon_oom_ctx_t *)handler->context;
    (void)old_level;
    (void)new_level;

    if (ctx->reject_requests_on_critical) {
        SVC_LOG_ERROR("P1.22: [%s] OOM CRITICAL — rejecting new requests", ctx->daemon_name);
    }

    return 0;
}

static int daemon_oom_on_critical_restore(degradation_handler_t *handler,
                                          watermark_level_t old_level, watermark_level_t new_level)
{
    daemon_oom_ctx_t *ctx = (daemon_oom_ctx_t *)handler->context;
    (void)old_level;
    (void)new_level;

    SVC_LOG_INFO("P1.22: [%s] OOM CRITICAL restored — accepting requests", ctx->daemon_name);
    return 0;
}

int daemon_oom_register(const daemon_oom_config_t *config)
{
    if (!config || !config->daemon_name)
        return AIRY_ERR_INVALID_PARAM;

    if (g_daemons_oom_count >= DAEMON_OOM_MAX_SLOTS) {
        SVC_LOG_ERROR("P1.22: No more daemon OOM slots available");
        return AIRY_ERR_OVERFLOW;
    }

    daemon_oom_ctx_t *ctx = &g_daemons_oom_slots[g_daemons_oom_count];
    AIRY_MEMSET(ctx, 0, sizeof(*ctx));

    AIRY_STRNCPY_TERM(ctx->daemon_name, config->daemon_name, sizeof(ctx->daemon_name) - 1);
    ctx->drop_cache_on_warning = config->drop_cache_on_warning;
    ctx->reject_requests_on_critical = config->reject_requests_on_critical;
    ctx->user_context = config->user_context;

    ctx->warning_handler.feature_name = ctx->daemon_name;
    ctx->warning_handler.trigger_level = WATERMARK_WARNING;
    ctx->warning_handler.action = DEGRADE_REDUCE_CACHE;
    ctx->warning_handler.on_degrade = daemon_oom_on_warning;
    ctx->warning_handler.on_restore = daemon_oom_on_warning_restore;
    ctx->warning_handler.context = ctx;

    airy_err_t err = airy_register_degradation(&ctx->warning_handler);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_WARN("P1.22: [%s] WARNING handler registration failed (err=%d)",
                     config->daemon_name, err);
    }

    ctx->critical_handler.feature_name = ctx->daemon_name;
    ctx->critical_handler.trigger_level = WATERMARK_CRITICAL;
    ctx->critical_handler.action = DEGRADE_REJECT_NEW_CONN;
    ctx->critical_handler.on_degrade = daemon_oom_on_critical;
    ctx->critical_handler.on_restore = daemon_oom_on_critical_restore;
    ctx->critical_handler.context = ctx;

    err = airy_register_degradation(&ctx->critical_handler);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_WARN("P1.22: [%s] CRITICAL handler registration failed (err=%d)",
                     config->daemon_name, err);
    }

    g_daemons_oom_count++;
    SVC_LOG_INFO("P1.22: [%s] OOM callbacks registered (warning=%s, critical=%s)",
                 config->daemon_name, config->drop_cache_on_warning ? "drop-cache" : "none",
                 config->reject_requests_on_critical ? "reject-requests" : "none");

    return 0;
}

void daemon_oom_unregister(const char *daemon_name)
{
    if (!daemon_name)
        return;

    for (int i = 0; i < g_daemons_oom_count; i++) {
        if (strcmp(g_daemons_oom_slots[i].daemon_name, daemon_name) == 0) {
            airy_unregister_degradation(&g_daemons_oom_slots[i].warning_handler);
            airy_unregister_degradation(&g_daemons_oom_slots[i].critical_handler);
            SVC_LOG_INFO("P1.22: [%s] OOM callbacks unregistered", daemon_name);
            return;
        }
    }
}
