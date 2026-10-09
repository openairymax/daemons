// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file svc.c
 * @brief think_d 生命周期策略域（config 加载链 / 服务装配 / 依赖治理）。
 *
 * 端点族委托 daemon_cfg_file 机制件（daemon_ep_load/free/apply）；策略
 * 键经 cfg_keys 自持提取 think 段。策略优先级（Model SSoT）：
 * env (AIRY_THINK_*) > model.yaml think 段 > -c JSON think/daemon 段 >
 * 内置缺省——daemon_ep_load（JSON）先行，model.yaml 与 env 顺序在后。
 */

#include "airy_memory.h"
#include "cog_review_strategy.h"
#include "cognitive_review.h"
#include "daemon_cfg_file.h"
#include "svc_think_d.h"
#include "think_d_internal.h"
#include "lang_svc.h"
#include "review_svc.h"
#include "platform.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

think_service_t *g_svc = NULL;

sd_helper_t *g_sdh = NULL;

const daemon_dep_spec_t g_think_deps[1] = {
    {"llm_d", true},
};

/* 端点三元组由 daemon_cfg_file 机制件持有；此处只留 think 策略键 */
typedef struct {
    uint32_t process_timeout_ms;
    int think_enabled;
    char think2_slow_model[128];
    char think1_fast_model[128];
    char think1_prof_model[128];
} think_daemon_config_t;

static think_daemon_config_t g_cfg = {0};

/* 生态认知审查 ops provider 注表（M5-4 C2）：只读转发 products/cognition
 * 策略载荷，与契约签名 1:1。注入后机制核 engine_phase0 经
 * are_ops_get_cpr() 分发认知并行审查（CPR）；未注入（NULL）时
 * 静默旁路，不阻断推理主链。 */
static const cog_review_ops_t g_cog_review_ops = {
    .run = cog_review_run,
    .result_init = cog_review_result_init,
    .result_free = cog_review_result_free,
};

/* 策略键派发（daemon_ep_load 回调）：think 段五键自持提取（JSON 层） */
static void cfg_keys(const cJSON *root, void *ud)
{
    (void)ud;
    cJSON *think = cJSON_GetObjectItem(root, "think");
    if (think) {
        cJSON *s2 = cJSON_GetObjectItem(think, "think2_slow_model");
        if (cJSON_IsString(s2) && s2->valuestring[0])
            AIRY_STRNCPY_TERM(g_cfg.think2_slow_model, s2->valuestring,
                              sizeof(g_cfg.think2_slow_model));
        cJSON *t1f = cJSON_GetObjectItem(think, "think1_fast_model");
        if (cJSON_IsString(t1f) && t1f->valuestring[0])
            AIRY_STRNCPY_TERM(g_cfg.think1_fast_model,
                              t1f->valuestring,
                              sizeof(g_cfg.think1_fast_model));
        cJSON *t1p = cJSON_GetObjectItem(think, "think1_prof_model");
        if (cJSON_IsString(t1p) && t1p->valuestring[0])
            AIRY_STRNCPY_TERM(g_cfg.think1_prof_model,
                              t1p->valuestring,
                              sizeof(g_cfg.think1_prof_model));
        cJSON *timeout = cJSON_GetObjectItem(think, "timeout_ms");
        if (cJSON_IsNumber(timeout))
            g_cfg.process_timeout_ms = (uint32_t)timeout->valueint;
        cJSON *enabled = cJSON_GetObjectItem(think, "enabled");
        if (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled))
            g_cfg.think_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
    }
}

static void cfg_load(const char *config_path)
{
    g_cfg.process_timeout_ms = 120000;
    g_cfg.think_enabled = 1;

    /* 机制件：端点基线 + config 文件覆盖 + cfg_keys 派发（JSON 层，
     * 优先级低于下方 model.yaml SSoT 与 env 链） */
    daemon_ep_load(daemon_ep_slot(), config_path, THINK_D_SOCKET_UNIX,
                   THINK_D_SOCKET_WIN, THINK_D_TCP_PORT, cfg_keys, NULL);

    /* Model SSoT: $AIRY_CONFIG_DIR/model.yaml 的 think 段（三角色单一
     * 配置源；与 gateway_d 读全局段同一 svc_model_defaults 公共层）。 */
    {
        char model_path[1024];
        const char *cfg_dir = airy_config_dir();
        int have_model_yaml = 0;
        if (cfg_dir) {
            snprintf(model_path, sizeof(model_path), "%s/model.yaml", cfg_dir);
            have_model_yaml = 1;
        }
        svc_model_think_config_t think_cfg;
        AIRY_MEMSET(&think_cfg, 0, sizeof(think_cfg));
        think_cfg.enabled = g_cfg.think_enabled;
        if (have_model_yaml && svc_model_defaults_think_from_yaml(model_path, &think_cfg) == 0) {
            g_cfg.think_enabled = think_cfg.enabled;
            if (think_cfg.think2_slow_model[0])
                AIRY_STRNCPY_TERM(g_cfg.think2_slow_model, think_cfg.think2_slow_model,
                                  sizeof(g_cfg.think2_slow_model));
            if (think_cfg.think1_fast_model[0])
                AIRY_STRNCPY_TERM(g_cfg.think1_fast_model, think_cfg.think1_fast_model,
                                  sizeof(g_cfg.think1_fast_model));
            if (think_cfg.think1_prof_model[0])
                AIRY_STRNCPY_TERM(g_cfg.think1_prof_model, think_cfg.think1_prof_model,
                                  sizeof(g_cfg.think1_prof_model));
            if (think_cfg.timeout_ms > 0)
                g_cfg.process_timeout_ms = think_cfg.timeout_ms;
        }

        const char *e;
        if ((e = getenv("AIRY_THINK_ENABLED")) && *e) {
            int b = (strcmp(e, "0") == 0 || strcasecmp(e, "false") == 0 ||
                     strcasecmp(e, "no") == 0) ?
                        0 :
                        1;
            g_cfg.think_enabled = b;
        }
        if ((e = getenv("AIRY_THINK2_SLOW_MODEL")) && *e)
            AIRY_STRNCPY_TERM(g_cfg.think2_slow_model, e, sizeof(g_cfg.think2_slow_model));
        if ((e = getenv("AIRY_THINK1_FAST_MODEL")) && *e)
            AIRY_STRNCPY_TERM(g_cfg.think1_fast_model, e, sizeof(g_cfg.think1_fast_model));
        if ((e = getenv("AIRY_THINK1_PROF_MODEL")) && *e)
            AIRY_STRNCPY_TERM(g_cfg.think1_prof_model, e, sizeof(g_cfg.think1_prof_model));
        if ((e = getenv("AIRY_THINK_TIMEOUT_MS")) && *e && atoi(e) > 0)
            g_cfg.process_timeout_ms = (uint32_t)atoi(e);
    }
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_apply(ep, cmdline_tcp);
}

int svc_prepare(const char *config_path)
{
    airy_paths_init();
    cfg_load(config_path);

    think_service_config_t svc_cfg;
    AIRY_MEMSET(&svc_cfg, 0, sizeof(svc_cfg));
    svc_cfg.enabled = g_cfg.think_enabled;
    svc_cfg.think2_slow_model = g_cfg.think2_slow_model[0] ? g_cfg.think2_slow_model : NULL;
    svc_cfg.think1_fast_model = g_cfg.think1_fast_model[0] ? g_cfg.think1_fast_model : NULL;
    svc_cfg.think1_prof_model = g_cfg.think1_prof_model[0] ? g_cfg.think1_prof_model : NULL;
    svc_cfg.process_timeout_ms = g_cfg.process_timeout_ms;

    g_svc = think_service_create(&svc_cfg);
    if (!g_svc) {
        SVC_LOG_ERROR("Failed to create think service");
        return -1;
    }

    /* M1-1c：推理语言网关服务面初始化（lang_gateway 懒创建，此处仅 mutex） */
    if (lang_svc_init() != 0)
        SVC_LOG_WARN("lang_svc_init failed, think.lang_* unavailable");

    /* M1-1c：执行复核服务面初始化（t2/t1-f 语义判断策略收拢到 think_d） */
    if (review_svc_init(think_service_llm_adapter(g_svc), g_cfg.think2_slow_model,
                        g_cfg.think1_fast_model) != 0)
        SVC_LOG_WARN("review_svc_init failed, think.review unavailable");

    /* M5-4 C2：注入认知审查 ops（products/cognition 策略载荷），机制核
     * engine_phase0 经 are_ops_get_cpr() 分发 CPR。 */
    are_ops_set_cpr(&g_cog_review_ops);

    SVC_LOG_INFO("think service started (enabled=%d, timeout_ms=%u)", g_cfg.think_enabled,
                 g_cfg.process_timeout_ms);
    return 0;
}

int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)driver;

    /* 0.1.19 §7.2 不变式 3：启动期依赖检查（governance 面职责）。缓存 SD
     * 探测句柄（RPC 域健康面共用），此处经 SD 探测声明的硬依赖；required
     * 缺失时 WARN + hall issue 事件显式上报，禁止静默 ready。 */
    g_sdh = daemon_bootstrap_sd_get_helper(bsd);
    daemon_dep_t deps;
    if (daemon_dep_init(&deps, g_think_deps, sizeof(g_think_deps) / sizeof(g_think_deps[0])) ==
            AIRY_SUCCESS &&
        daemon_dep_probe(&deps, g_sdh) == AIRY_SUCCESS) {
        (void)daemon_dep_report(&deps, "think_d");
    }
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    /* M1-1c：先释放复核/语言网关服务面对 svc 的引用，再销毁本体 */
    are_ops_set_cpr(NULL);
    review_svc_cleanup();
    lang_svc_cleanup();
    if (g_svc) {
        think_service_destroy(g_svc);
        g_svc = NULL;
    }
    daemon_ep_free(daemon_ep_slot());
}

void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
