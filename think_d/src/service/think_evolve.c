/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file think_evolve.c
 * @brief 演化观测面采样实现（0.1.20 W4）。
 *
 * 采样链：orchestrator_evolution_stats() → 策略侧 mc.stats JSON →
 * think_evo_map() 归一为标量 → observe_record_metric 推送 monit_d。
 * counter 类指标上行累计差值，基线仅在推送成功后推进，避免 monit_d
 * 短暂不可达造成计数永久丢失。采样失败仅告警，不阻断推理主链。
 */

#include "think_evolve.h"

#include "think_orch_internal.h"

#include "airy_ipc_ops.h"
#include "orchestrator.h"
#include "platform.h"
#include "provider.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdio.h>

#define EVO_RPC_TIMEOUT_MS 2000
#define EVO_PARAMS_CAP 256

typedef struct {
    const char *name;
    double value;
    const char *type;
    const char *unit;
} evo_metric_t;

/* 采样源：baseline 非空者为 counter（上行差值），为空者为 gauge。 */
typedef struct {
    const char *name;
    const char *type;
    const char *unit;
    double raw;
    double *baseline;
} evo_src_t;

static double g_last_detections;
static double g_last_preemptions;
static double g_last_noop;

static double evo_num(const cJSON *obj, const char *key)
{
    const cJSON *item = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return (item && cJSON_IsNumber(item)) ? item->valuedouble : 0.0;
}

int think_evo_map(const char *mc_json, uint64_t noop, think_evo_snapshot_t *out)
{
    if (!mc_json || !out)
        return -1;

    cJSON *root = cJSON_Parse(mc_json);
    if (!root)
        return -1;

    *out = (think_evo_snapshot_t){0};
    const cJSON *learning = cJSON_GetObjectItemCaseSensitive(root, "learning");
    out->patterns = evo_num(learning, "patterns");
    out->detections = evo_num(learning, "detected");
    out->preemptions = evo_num(learning, "preemptive");
    out->effectiveness = evo_num(learning, "effectiveness");
    out->records = evo_num(root, "records");
    out->evolve_noop = (double)noop;
    cJSON_Delete(root);
    return 0;
}

double think_evo_delta(double cur, double last)
{
    if (cur < last)
        return cur;
    return cur - last;
}

static int evo_push(const evo_metric_t *m)
{
    const airy_ipc_ops_t *ops = are_ops_get_ipc();
    char params[EVO_PARAMS_CAP];
    char *result = NULL;
    int n, rc;

    if (!ops || !ops->rpc_call)
        return -1;

    n = snprintf(params, sizeof(params),
                 "{\"name\":\"%s\",\"value\":%.6f,\"type\":\"%s\",\"unit\":\"%s\"}",
                 m->name, m->value, m->type, m->unit);
    if (n < 0 || (size_t)n >= sizeof(params))
        return -1;

    rc = ops->rpc_call(airy_runtime_dir_socket("monit.sock"), "observe_record_metric", params,
                       &result, EVO_RPC_TIMEOUT_MS);
    AIRY_FREE(result);
    return rc == 0 ? 0 : -1;
}

static void evo_tick(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;

    think_service_t *svc = (think_service_t *)user_data;
    think_evo_snapshot_t snap;
    char *json = NULL;
    size_t pushed = 0;
    int mapped;

    if (!svc || !svc->orch)
        return;
    if (orchestrator_evolution_stats(svc->orch, &json) != 0 || !json) {
        AIRY_FREE(json);
        return;
    }

    mapped = think_evo_map(json, airy_memrpc_evolve_noop_count(), &snap);
    AIRY_FREE(json);
    if (mapped != 0)
        return;

    evo_src_t src[6] = {
        {"airy_evolution_patterns", "gauge", "count", snap.patterns, NULL},
        {"airy_evolution_detections_total", "counter", "count", snap.detections,
         &g_last_detections},
        {"airy_evolution_preemptions_total", "counter", "count", snap.preemptions,
         &g_last_preemptions},
        {"airy_evolution_effectiveness", "gauge", "ratio", snap.effectiveness, NULL},
        {"airy_evolution_records", "gauge", "count", snap.records, NULL},
        {"airy_evolution_evolve_noop_total", "counter", "count", snap.evolve_noop, &g_last_noop},
    };
    const size_t total = sizeof(src) / sizeof(src[0]);

    for (size_t i = 0; i < total; i++) {
        evo_src_t *s = &src[i];
        evo_metric_t m = {s->name,
                          s->baseline ? think_evo_delta(s->raw, *s->baseline) : s->raw,
                          s->type, s->unit};
        if (evo_push(&m) != 0)
            continue;
        pushed++;
        /* 基线只随该条成功推送推进：monit_d 短暂不可达时增量在下次补齐。 */
        if (s->baseline)
            *s->baseline = s->raw;
    }

    if (pushed < total)
        SVC_LOG_WARN("think_evolve: %zu/%zu metrics pushed (monit_d unreachable?)", pushed,
                     total);
}

int think_evolve_start(daemon_event_driver_t *driver, think_service_t *svc)
{
    if (!driver || !svc)
        return -1;
    if (daemon_event_driver_add_timer(driver, THINK_EVOLVE_INTERVAL_MS, evo_tick, svc) == 0)
        return -1;
    SVC_LOG_INFO("think_evolve: evolution sampling started (interval=%u ms)",
                 THINK_EVOLVE_INTERVAL_MS);
    return 0;
}
