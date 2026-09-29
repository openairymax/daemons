// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file svc.c
 * @brief think_d 生命周期策略域（config 加载链 / 服务装配 / 依赖治理）。
 *
 * config 优先级（Model SSoT）：env (AIRY_THINK_*) > model.yaml think 段 >
 * -c JSON think/daemon 段 > 内置缺省。
 */

#include "airy_memory.h"
#include "svc_think_d.h"
#include "think_d_internal.h"
#include "lang_svc.h"
#include "review_svc.h"
#include "platform.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"
#include "daemon_main.h"

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

typedef struct {
    char *socket_path;
    uint16_t tcp_port;
    int use_tcp;
    uint32_t process_timeout_ms;
    int think_enabled;
    char think2_slow_model[128];
    char think1_fast_model[128];
    char think1_prof_model[128];
} think_daemon_config_t;

static think_daemon_config_t g_cfg = {0};

static void cfg_load(const char *config_path)
{
    g_cfg.use_tcp = 0;
    g_cfg.process_timeout_ms = 120000;
    g_cfg.think_enabled = 1;
    g_cfg.tcp_port = THINK_D_TCP_PORT;
#if defined(AIRY_PLATFORM_WINDOWS)
    g_cfg.socket_path = AIRY_STRDUP(THINK_D_SOCKET_WIN);
#else
    g_cfg.socket_path = AIRY_STRDUP(THINK_D_SOCKET_UNIX);
#endif

    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0 && len < 1024 * 1024) {
                char *content = (char *)AIRY_MALLOC((size_t)len + 1);
                if (content) {
                    size_t read_len = fread(content, 1, (size_t)len, f);
                    if (read_len == (size_t)len) {
                        content[read_len] = '\0';
                        do {
                            CJSON_PARSE_GUARD(root, content, { break; });
                            cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
                            if (daemon_cfg) {
                                cJSON *socket_path =
                                    cJSON_GetObjectItem(daemon_cfg, "socket_path");
                                if (cJSON_IsString(socket_path)) {
                                    AIRY_FREE(g_cfg.socket_path);
                                    g_cfg.socket_path = AIRY_STRDUP(socket_path->valuestring);
                                }
                                cJSON *tcp_port = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                                if (cJSON_IsNumber(tcp_port)) {
                                    g_cfg.tcp_port = (uint16_t)tcp_port->valueint;
                                    g_cfg.use_tcp = 1;
                                }
                            }
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
                        } while (0);
                    }
                    AIRY_FREE(content);
                }
            }
            fclose(f);
        }
    }

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

static void cfg_free(void)
{
    AIRY_FREE(g_cfg.socket_path);
    AIRY_MEMSET(&g_cfg, 0, sizeof(g_cfg));
}

void svc_endpoint_think_d(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp ? 1 : (g_cfg.use_tcp ? 1 : 0);
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = g_cfg.tcp_port;
    ep->sock_unix = g_cfg.socket_path;
    ep->sock_win = g_cfg.socket_path;
}

int svc_prepare_think_d(const char *config_path)
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

    SVC_LOG_INFO("think service started (enabled=%d, timeout_ms=%u)", g_cfg.think_enabled,
                 g_cfg.process_timeout_ms);
    return 0;
}

int svc_activate_think_d(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
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

void svc_teardown_think_d(void)
{
}

void svc_destroy_think_d(void)
{
    /* M1-1c：先释放复核/语言网关服务面对 svc 的引用，再销毁本体 */
    review_svc_cleanup();
    lang_svc_cleanup();
    if (g_svc) {
        think_service_destroy(g_svc);
        g_svc = NULL;
    }
    cfg_free();
}

void svc_attach_think_d(void *dispatcher)
{
    (void)dispatcher;
}
