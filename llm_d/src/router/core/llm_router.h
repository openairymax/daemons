/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_router.h
 * @brief LLM router interface.
 *
 * The LLM Router routes LLM requests to the most suitable provider and
 * model based on request characteristics (complexity, cost, latency, etc.).
 *
 * Routing strategies (selection-level only: pick an endpoint before the call):
 *   - COMPLEXITY_BASED:  route by task complexity
 *   - COST_OPTIMIZED:    cost-optimized routing
 *   - LATENCY_OPTIMIZED: latency-optimized routing
 *   - FALLBACK:          selection fallback (re-select with another strategy
 *                        when no endpoint is eligible; NOT call-level failover)
 *   - ROUND_ROBIN:       round-robin routing
 *
 * 执行期故障切换不在此处：失败后换端点见 rpc/service_chain.h 与
 * router/core/llm_failover.h（0.1.19 §3.8，证伪 H2）。
 *
 * 端点健康面（0.1.19 §3.8.2）同属本契约：llm_endpoint_t 的四个健康字段
 * 只由 llm_ep_* 迁移，使「端点还能不能用」成为一处可判定、可单测的规则。
 * 健康面与选择面同头，是因为二者共享同一 llm_endpoint_t 值语义；拆成两个
 * 头会让调用方为一次判定同时摘两件，白白抬高扇入（0.1.19 §4.4 五件套化）。
 *
 *   CLOSED --窗口内连续失败达 LLM_EP_FAIL_MAX，或 AUTH 一次--> OPEN
 *   OPEN   --open_until_ms 过期--> HALF，只放行单次探测
 *   HALF   --探测成功--> CLOSED；连续失败达 LLM_EP_PROBE_MAX--> OPEN
 *
 * CLOSED 期的连续失败计数按滑动窗口衰减：距上次失败已满
 * LLM_EP_FAIL_WINDOW_MS 的失败不再与前一轮累计。每次自 HALF 转入 OPEN
 * 都抬一档退避（LLM_EP_OPEN_MS 起，按 2 的幂递增，封顶
 * LLM_EP_OPEN_MAX_MS），探测成功归 CLOSED 时归零档位。
 *
 * @owner team-A
 * @see contracts/contract_A_B.h section 3 (protocol routing table)
 */

#ifndef AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H
#define AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H

#include "airy_abi.h"
#include "llm_service_types.h"

AIRY_ABI_BEGIN

/* 路由成本估算用的输出 token 兜底值。调用方（service 层）已把引擎默认
 * max_output 解析进 llm_route_request_t.max_tokens；仅当配置面完全未声明
 * 输出上限时才落到此值。四个策略必须共用同一口径，否则同一请求在不同
 * 策略下会得到不同的成本预期。 */
#define LLM_ROUTE_EST_OUTPUT_TOKENS 1024U

typedef enum {
    LLM_ROUTE_COMPLEXITY = 0,
    LLM_ROUTE_COST = 1,
    LLM_ROUTE_LATENCY = 2,
    LLM_ROUTE_FALLBACK = 3,
    LLM_ROUTE_ROUND_ROBIN = 4,
    LLM_ROUTE_COUNT = 5
} llm_route_strategy_t;

typedef enum {
    LLM_CAP_CHAT = 0x0001,
    LLM_CAP_COMPLETION = 0x0002,
    LLM_CAP_EMBEDDING = 0x0004,
    LLM_CAP_FUNCTION_CALL = 0x0008,
    LLM_CAP_VISION = 0x0010,
    LLM_CAP_STREAMING = 0x0020,
    LLM_CAP_JSON_MODE = 0x0040,
    LLM_CAP_EXTENDED_THINK = 0x0080,
    LLM_CAP_CODE_EXEC = 0x0100
} llm_capability_t;

typedef struct {
    char provider_name[64];
    char model_name[64];
    char endpoint[256];
    char api_key_env[64];
    uint32_t capabilities;
    uint32_t context_window;
    double cost_per_1k_input;
    double cost_per_1k_output;
    uint32_t avg_latency_ms;
    bool enabled;
    int priority;

    /* 端点健康面（0.1.19 §3.8.2）：仅 llm_d 内部可见，注册时清零，迁移
     * 规则见本头文件「端点健康面」段。计时为 airy_time_ms() 截断到 32 位，
     * 以无符号差比较抵御回绕，同时避免 32 位平台上的 64 位读撕裂。 */
    uint32_t consec_fail;
    uint32_t last_fail_ms;
    uint32_t open_until_ms;
    uint32_t probe_until_ms;
    uint32_t probe_fail;
    uint8_t open_level;
} llm_endpoint_t;

/** 熔断三态。 */
typedef enum {
    LLM_EP_CLOSED = 0,
    LLM_EP_OPEN = 1,
    LLM_EP_HALF = 2
} llm_ep_state_t;

/** 连续失败阈值：达此值转 OPEN。 */
#define LLM_EP_FAIL_MAX 3U

/** 连续失败计数窗口（毫秒）：距上次失败满窗后重新起算。 */
#define LLM_EP_FAIL_WINDOW_MS 60000U

/** OPEN 保持时长（毫秒），指数退避的首档。 */
#define LLM_EP_OPEN_MS 30000U

/** OPEN 保持时长上限（毫秒）。 */
#define LLM_EP_OPEN_MAX_MS 300000U

/** 指数退避最高档位：LLM_EP_OPEN_MS << 4 已超上限，封顶生效。 */
#define LLM_EP_MAX_LEVEL 4U

/** 半开探测最长占用（毫秒）：探测方未回报则释放槽位，允许重新探测。 */
#define LLM_EP_PROBE_MS 10000U

/** HALF 期连续探测失败阈值：达此值转 OPEN 并抬升退避档位。 */
#define LLM_EP_PROBE_MAX 3U

/** 由健康字段推导当前状态（now = airy_time_ms() 截断到 32 位）。 */
llm_ep_state_t llm_ep_state(const llm_endpoint_t *ep, uint32_t now);

/** HALF 状态下首个调用方占用探测槽。@return 1 放行，0 拒绝。 */
int llm_ep_admit(llm_endpoint_t *ep, uint32_t now);

/** 记录一次失败，按判据迁移状态。不可切换类错误不计入。 */
void llm_ep_fail(llm_endpoint_t *ep, int err, uint32_t now);

/** 记录一次成功：清零计数并归 CLOSED。 */
void llm_ep_ok(llm_endpoint_t *ep);

/**
 * 锁内按 provider/model 定位端点的包装；端点未注册或 router 未初始化时
 * 一律按拒绝方向处理。
 */
int llm_rtr_admit(const char *provider, const char *model);

/**
 * 端点是否已注册到健康面。
 *
 * 链式切换需要一个与「能否放行」不同的判据：llm_rtr_admit() 对未注册端点
 * 返回拒绝方向（0），据此判定会把 router 未初始化或端点超限时的候选全部
 * 跳过。本函数区分「未注册（按可用处理）」与「已注册但明确拒绝」。
 *
 * @return 1 已注册，0 未注册或 router 未初始化。
 */
int llm_rtr_known(const char *provider, const char *model);

/** 锁内记录失败。@return 0 已记录，负值未找到端点。 */
int llm_rtr_fail(const char *provider, const char *model, int err);

/** 锁内记录成功。@return 0 已记录，负值未找到端点。 */
int llm_rtr_ok(const char *provider, const char *model);

/**
 * @brief 上游整体健康三态（0.1.20 D2-5）。
 *
 * 进程存活与上游可用是两件事：前者由 IPC 应答证明，后者只能由端到端的
 * 探测/请求结果证明。health_check 报的是后者，故状态必须从端点健康面
 * 聚合得出，不得以服务句柄非空冒充。
 */
typedef enum {
    LLM_HEALTH_DOWN = 0,     /**< 无可用上游（全部熔断或未注册） */
    LLM_HEALTH_DEGRADED = 1, /**< 部分上游不可用，仍可服务 */
    LLM_HEALTH_OK = 2        /**< 全部启用端点 CLOSED */
} llm_health_state_t;

/** 聚合快照：total 为启用端点数，ok 为 CLOSED 数，down 为 OPEN/HALF 数。 */
typedef struct {
    llm_health_state_t state;
    size_t total;
    size_t ok;
    size_t down;
} llm_health_snap_t;

/**
 * @brief 锁内聚合端点健康面。
 * @param out 可为 NULL；非 NULL 时回填计数明细。
 * @return 聚合状态（router 未初始化或无启用端点时 DOWN）。
 */
llm_health_state_t llm_rtr_health(llm_health_snap_t *out);

/** 三态名的 wire 拼写（"ok"/"degraded"/"down"），用于 health_check 应答。 */
const char *llm_health_name(llm_health_state_t state);

/**
 * @brief 按 provider 记账一次端到端可达性观测（0.1.20 D2-5）。
 *
 * 主动探测只到 provider 粒度（/models + 深度探针），而健康面按
 * provider×model 记账：同 provider 的全部端点共享同一条上游链路，故一次
 * 观测对全部同名端点生效。失败按 AIRY_ERR_IO 记账——探测失败是传输面
 * 不可达，与调用期连接失败同属可切换类（llm_fo_reason 显式表）。
 *
 * @param provider 端点 provider 名。
 * @param ok       1 可达，0 不可达。
 * @return 命中的端点条数（0 表示 provider 未注册或 router 未初始化）。
 */
size_t llm_rtr_provider_note(const char *provider, int ok);

/**
 * 测量面单点记账（0.1.20 D0）：端点延迟 EWMA 回填 + stats 聚合。
 * 测量面与健康面分离——不触碰熔断四字段；熔断 off 模式同样回填，
 * 使 latency_skipped/least_latency 消费的 avg_latency_ms 恒为实测真值。
 * provider 为 NULL 表示缓存命中回放：只聚合 token 计数，不记延迟样本、
 * 不重复计 timings（响应来自历史请求的计量，不是本次上游往返）。
 * @return 0 已记账，负值参数非法或 router 未初始化。
 */
int llm_rtr_meas(const char *provider, const char *model, const llm_response_t *resp,
                 uint64_t lat_ms);

typedef struct {
    const char *prompt;
    size_t prompt_len;
    uint32_t required_caps;
    uint32_t max_tokens;
    double max_cost;
    uint32_t max_latency_ms;
    llm_route_strategy_t strategy;
    char preferred_provider[64];
} llm_route_request_t;

typedef struct {
    char provider_name[64];
    char model_name[64];
    char endpoint[256];
    double estimated_cost;
    uint32_t estimated_latency_ms;
    llm_route_strategy_t strategy_used;
    int confidence;
    char fallback_provider[64];
    char fallback_model[64];
} llm_route_result_t;

typedef struct {
    uint64_t total_requests;
    uint64_t routed_count[5];
    uint64_t fallback_count;
    uint64_t error_count;
    double total_cost;
    uint64_t total_tokens;
    /* 测量面聚合（0.1.20 D0/G47/G62）：lat_* 来自逐请求实测；meas_* 仅统计
     * 端点真实上报 timings 的请求，未上报者不入样本，防止分列均值被零值稀释。 */
    uint64_t lat_samples;
    uint64_t lat_total_ms;
    uint64_t prompt_tokens_sum;
    uint64_t cached_tokens_sum;
    uint64_t meas_samples;
    uint64_t prefill_tokens;
    uint64_t decode_tokens;
    uint64_t kv_cache_tokens;
    double prefill_ms;
    double decode_ms;
} llm_router_stats_t;

/** 初始化路由表。@return 0 成功，非 0 失败。 */
int llm_router_init(void);

/** 销毁路由表。 */
void llm_router_destroy(void);

/** 注册端点，健康字段清零。@return 0 成功，非 0 失败。 */
int llm_router_register_endpoint(const llm_endpoint_t *endpoint);

/**
 * @brief 清空端点表以便按新配置整表重注册，统计量保留。
 *
 * 注册是追加语义，热重载若只追加就会把同一端点重复计入、最终撞上
 * LLM_ROUTER_MAX_ENDPOINTS。重载方在排空窗口内调用本接口再重新注册，
 * 观测计数（请求、成本、错误）不因换配置而清零。
 *
 * @return 0 成功；AIRY_ERR_SYS_NOT_INIT 未初始化。
 */
int llm_router_reset_endpoints(void);

/** 按请求选路。@return 0 成功，非 0 失败。 */
int llm_router_route(const llm_route_request_t *request, llm_route_result_t *result);

/**
 * @brief 枚举本次请求的全部可达端点，按优先级降序、估算成本升序排列。
 *
 * 调用级链式切换（0.1.19 §3.8.4 A3）需要「下一个候选」而非「唯一最优」，
 * 且候选必须与 llm_router_route() 使用同一套可达性口径（熔断 OPEN 排除、
 * 能力位与 preferred_provider 过滤）。排序取 priority 降序、估算成本升序、
 * 注册序升序，使首选与 llm_router_route() 的 COST 策略一致。
 *
 * 本函数只读端点表，不加锁，与既有策略实现保持一致；调用方看到的是
 * 某一瞬间的快照，方向安全。
 *
 * @param request   路由请求。
 * @param out       结果数组，容量由 max 指定。
 * @param max       结果数组容量。
 * @param out_count 实际写出数量。
 * @return 0 成功（含无候选），AIRY_ERR_SYS_NOT_INIT 路由未初始化。
 */
int llm_router_pick(const llm_route_request_t *request, llm_route_result_t *out, size_t max,
                          size_t *out_count);

/**
 * @brief Get router statistics.
 * @param stats Output statistics
 * @return 0 on success, non-zero on failure
 */
int llm_router_get_stats(llm_router_stats_t *stats);

AIRY_ABI_END

#endif /* AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H */
