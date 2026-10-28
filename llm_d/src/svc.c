/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief llm_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * llm 服务单例 + 生命周期五钩子。可配置户：端点基线取自生成头常量
 * （LLM_D_SOCKET_UNIX/WIN、LLM_D_TCP_PORT），端点族委托 daemon_cfg_file
 * 机制件（daemon_ep_load/free/apply，纯端点户无策略键）。配置未显式
 * 给出时回落 $AIRY_CONFIG_DIR/model.yaml（与 think_d / gateway_d 同源
 * SSoT），使 provider 注册表与 llm_router 始终看到已配置端点。业务
 * 逻辑在 src/rpc/methods.c 与 src/rpc/dispatch.c。
 */

#include "platform.h"
#include "svc_logger.h"
#include "svc_llm_d.h"
#include "llm_d_internal.h"

#include "daemon_cfg_file.h"
#include "daemon_dep.h"
#include "daemon_event_driver.h"
#include "providers/core/ep_health.h"
#include "providers/core/registry.h"
#include "router/core/llm_router.h"
#include "svc_model_watch.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>

/* D2-9⑤ 端点健康探测周期：30 秒（架构改进方案 §5.2b.10）。探测本身在
 * daemon 线程池 worker 上执行，周期定时器只做一次 offload，不阻塞事件循环。 */
#define EP_PROBE_INTERVAL_MS 30000

/* D2-9⑤ 深度探针周期倍率：每 5 个轻探测周期追加一次 1-token 生成。
 * 轻探测只证明路由存在，深度探针才证明权重已加载、推理可用；倍率取值
 * 让生成请求不成为常态负载，同时把"路由在但推理挂"的发现延迟限制在
 * 2.5 分钟内。 */
#define EP_PROBE_DEEP_EVERY 5

/* D5 探测范围：仅本地低算力端点——远端云端点由其 SLA 与网关侧观测覆盖，
 * 无需 llm_d 额外出网探测（探测范围属策略，周期/路径/范围在此注入机制件）。
 * 该范围同时保证深度探针不会对云端产生计费请求。键 "local" 由机制件解释
 * 为 api_base 指向 loopback（位置事实），与 provider 用户命名解耦。 */
#define EP_PROBE_FILTER "local"
#define EP_PROBE_PATH "/models"

/* D2-6 降级上报：事件流分组键，与 daemon 名一致。 */
#define LLM_DEP_NAME "llm_d"

llm_service_t *g_service = NULL;

/* 端点健康视图单例（定义归装配域）：extern 声明在 llm_d_internal.h，
 * 供 methods.c 的 health_check 与流式冷加载上报只读消费。 */
ep_health_t *g_ep = NULL;

/* daemon 线程池句柄：定时器回调在事件循环线程执行，网络探测必须
 * offload 至此，避免阻塞事件循环。生命周期归 daemon 框架，本件只借用。 */
static thread_pool_t *g_ep_pool = NULL;

/* 探测轮次：只在事件循环线程递增，故无需原子；用于裁决本轮是否追加
 * 深度探针（首轮即 deep，让启动后的第一次结论就覆盖整条推理链）。 */
static unsigned g_ep_round = 0;

/* D2-6 上报闸门：边沿触发 + 同态 60s 抑制，避免每请求一条事件。 */
static daemon_dep_gate_t g_hl_gate;

/* D2-5 汇聚出口：探测结论喂进端点健康唯一状态机（router 面）。
 * 返回命中端点条数（0 表示该 provider 不在 router 端点表内）。 */
static void ep_note_router(void *ud, const char *provider, int reachable)
{
    (void)ud;
    (void)llm_rtr_prov_note(provider, reachable);
}

/* D2-6：把聚合三态按边沿规则上报。读取的是状态机当前值，不产生额外探测，
 * 故可安全地在每轮探测末尾调用。 */
static void ep_report(void)
{
    llm_health_snap_t snap;
    llm_health_state_t st = llm_rtr_health(&snap);
    char detail[96];
    int state;

    if (st == LLM_HEALTH_OK)
        state = DAEMON_DEP_ST_OK;
    else if (st == LLM_HEALTH_DEGRADED)
        state = DAEMON_DEP_ST_DEGRADED;
    else
        state = DAEMON_DEP_ST_DOWN;

    (void)snprintf(detail, sizeof(detail), "upstream %zu/%zu ok", snap.ok, snap.total);
    (void)daemon_dep_state(&g_hl_gate, LLM_DEP_NAME, state, detail, airy_time_ms());
}

static void ep_probe_task(void *arg)
{
    ep_health_sweep(g_ep, (int)(uintptr_t)arg);
    ep_report();
}

/* 周期定时器回调（事件循环线程）：只做一次 offload，绝不在此出网。
 * 线程池不可用（未配置 pool_max）时降级为就地探测——探测量级为端点
 * 数量级、单发超时 8s，远小于周期，降级仅推迟该轮结果不丢结论。 */
static void ep_probe_tick(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;
    (void)user_data;

    if (!g_ep)
        return;

    int deep = (g_ep_round++ % EP_PROBE_DEEP_EVERY) == 0;
    void *arg = (void *)(uintptr_t)deep;

    if (g_ep_pool) {
        if (thread_pool_submit(g_ep_pool, ep_probe_task, arg) != 0)
            SVC_LOG_WARN("C-L02: EP-HEALTH: probe offload rejected, skipping this round");
        return;
    }
    ep_probe_task(arg);
}

static void ep_load(const char *config_path)
{
    daemon_ep_load(daemon_ep_slot(), config_path, LLM_D_SOCKET_UNIX,
                   LLM_D_SOCKET_WIN, LLM_D_TCP_PORT, NULL, NULL);
}

/* D2-7 配置热重载：SIGHUP 是唯一应用入口，周期 tick 只检测。
 *
 * 信号处理器只置位（async-signal-safe）；重载在事件循环线程的周期回调里
 * offload 到 daemon 线程池——排空最长数十秒，绝不能在事件循环里做。线程
 * 池不可用时就地执行：宁可短暂阻塞，也不丢一次显式请求。 */
static volatile sig_atomic_t g_cfg_hup = 0;

#ifndef _WIN32
static void cfg_on_hup(int sig)
{
    (void)sig;
    g_cfg_hup = 1;
}
#endif

static void cfg_reload(void *arg)
{
    (void)arg;
    int rc = llm_svc_cfg_hup(g_service);
    if (rc < 0)
        SVC_LOG_WARN("C-L02: CFG: reload failed — current config kept");
    else if (rc == 1)
        SVC_LOG_INFO("C-L02: CFG: reload skipped (in progress or backoff)");
}

static void cfg_tick(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;
    (void)user_data;

    if (g_cfg_hup) {
        if (!g_ep_pool) {
            g_cfg_hup = 0;
            cfg_reload(NULL);
            return;
        }
        if (thread_pool_submit(g_ep_pool, cfg_reload, NULL) == 0) {
            g_cfg_hup = 0;
            return;
        }
        /* 置位保留：下一周期重试，显式重载请求不丢 */
        SVC_LOG_WARN("C-L02: CFG: reload offload rejected — retry next tick");
        return;
    }
    if (g_service)
        (void)llm_svc_cfg_tick(g_service, airy_time_ms());
}

/* 与端点探测解耦注册：探测视图建立失败不得连带停掉配置感知，故本函数
 * 在 ep_health_create 之前调用。 */
static void cfg_arm(daemon_event_driver_t *driver)
{
#ifndef _WIN32
    if (signal(SIGHUP, cfg_on_hup) == SIG_ERR)
        SVC_LOG_WARN("C-L02: CFG: SIGHUP handler install failed — reload needs restart");
#endif
    daemon_event_driver_add_timer(driver, SVC_MW_INTERVAL_MS, cfg_tick, NULL);
    SVC_LOG_INFO("C-L02: CFG: drift watch armed (interval=%ums)", SVC_MW_INTERVAL_MS);
}

/* 未显式指定 manager 时回落 $AIRY_CONFIG_DIR/model.yaml：静态缓冲保持
 * 进程生命周期有效（llm_service 不复制该路径）。 */
static const char *cfg_fallback(const char *config_path)
{
    static char default_model_path[1024];
    if (config_path)
        return config_path;

    const char *cfg_dir = airy_config_dir();
    if (!cfg_dir)
        return NULL;

    int plen = snprintf(default_model_path, sizeof(default_model_path), "%s/model.yaml", cfg_dir);
    if (plen <= 0 || plen >= (int)sizeof(default_model_path))
        return NULL;
    return default_model_path;
}

int svc_prepare(const char *config_path)
{
    /* LLM_D_SOCKET_UNIX 展开为 airy_runtime_dir_socket()，且 cfg_fallback
     * 依赖 airy_config_dir()：路径体系须先就绪（与 think_d/gateway_d 同序）。 */
    airy_paths_init();

    const char *cfg = cfg_fallback(config_path);
    ep_load(cfg);

    SVC_LOG_INFO("LLM service starting, manager=%s", cfg ? cfg : "default");

    g_service = llm_service_create(cfg);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create service");
        return -1;
    }
    SVC_LOG_INFO("LLM service started");
    return 0;
}

/* D5：事件驱动就绪后的策略挂点。装配顺序保证 svc_prepare 已建 g_service，
 * 故此处可经 llm_svc_reg(g_service) 取 registry 快照建探测视图。探测周期
 * 定时器与 KEEP_ALIVE 建议共同回应"能力存在 ≠ 能力可用"（§6 阻断 3）。 */
int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)bsd;

    g_ep_pool = daemon_event_driver_get_pool(driver);

    /* 配置感知先于探测视图注册：探测失败会 return 0 提前退出，若放在其
     * 后，热重载会随探测一起被静默跳过（D2-7 硬要求：空闲进程也要能报
     * 漂移、能收 SIGHUP）。 */
    cfg_arm(driver);

    g_ep = ep_health_create(llm_svc_reg(g_service), EP_PROBE_FILTER, EP_PROBE_PATH);
    if (!g_ep) {
        /* 探测属观测能力，非服务前置：视图不可用时降级禁用探测，绝不因此
         * 中止 llm_d 启动（daemon_boot 对 svc_activate 非 0 值会 abort 整个
         * 服务）。status 侧对缺失视图诚实报"探测不可用"。 */
        SVC_LOG_ERROR("C-L02: EP-HEALTH: endpoint view unavailable, health probes disabled");
        return 0;
    }

    /* D2-5：探测结论经 sink 汇入端点健康唯一状态机。接线必须先于首轮
     * 探测，否则启动首轮结论只在视图内、不进状态机。 */
    ep_health_sink(g_ep, ep_note_router, NULL);

    /* 首轮探测立即触发：status 在启动后即能反映真实可达性，而非等待
     * 一个完整周期后才由 unknown 转为结论；首轮同时执行深度探针。 */
    ep_probe_tick(NULL, 0, NULL);

    daemon_event_driver_add_timer(driver, EP_PROBE_INTERVAL_MS, ep_probe_tick, NULL);
    SVC_LOG_INFO("C-L02: EP-HEALTH: probe timer registered (interval=%dms, deep_every=%d, "
                 "filter=%s)",
                 EP_PROBE_INTERVAL_MS, EP_PROBE_DEEP_EVERY, EP_PROBE_FILTER);
    return 0;
}

void svc_destroy(void)
{
    /* 先复原 SIGHUP 默认处置：处理器只置位一个静态标志，销毁后若仍有信号
     * 到达，指向的语义已不存在；测试框架与重启路径也不该继承本进程处置。 */
#ifndef _WIN32
    (void)signal(SIGHUP, SIG_DFL);
#endif
    g_cfg_hup = 0;

    /* daemon_cleanup 先销毁事件驱动（含线程池 join + 定时器），后调
     * svc_destroy：此刻已无在飞探测、无定时器回调，销毁视图无并发读。 */
    if (g_ep) {
        ep_health_destroy(g_ep);
        g_ep = NULL;
    }
    g_ep_pool = NULL;

    if (g_service) {
        llm_service_destroy(g_service);
        g_service = NULL;
    }
    daemon_ep_free(daemon_ep_slot());
}
