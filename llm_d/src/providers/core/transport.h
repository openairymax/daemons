/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file transport.h
 * @brief Provider 域机制层（B16-S3 拆分自 provider.h）。
 *
 * 出网传输、重试谓词、限流、请求骨架与响应解析的唯一实现（SSoT）。
 * 适配层（adapters/）只实现 core/adapter.h 契约，禁止自持本层任何
 * 副本；本头禁止出现厂商特判分支。
 */

#ifndef LLM_D_PROVIDERS_CORE_TRANSPORT_H
#define LLM_D_PROVIDERS_CORE_TRANSPORT_H

/* B16-S2 内层反依赖：providers 只依赖跨层类型契约（commons SSoT），
 * 不依赖发布头 llm_service.h——发布头仅 rpc 域门面可见。 */
#include "llm_service_types.h"

#include "airy_memory.h"

#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 限流器在本层只作不透明句柄（provider_request_t.rl）：完整定义在
 * core/rate_limit.h，此处前置声明以避免 transport → rate_limit 的头文件环。 */
struct provider_rate_limiter;

typedef struct {
    char api_key[256];
    char api_key_env[128]; /* model.yaml api_key_env name (e.g. DEEPSEEK_API_KEY);
                            * hot-loaded from secrets.env per request, so keys
                            * filled after startup need no restart */
    char api_base[512];
    char organization[128];
    double timeout_sec;
    int max_retries;
} provider_base_ctx_t;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} provider_http_resp_t;

/* 鉴权头样式（B16-S3.1 SSoT）：适配层只声明用哪一种，鉴权头的拼装、
 * 缓冲擦除与头链释放全部由 core 接管——适配层因此无需也不得调用任何
 * curl_* 接口，"适配层零 I/O"由此获得结构保证。 */
typedef enum {
    PROVIDER_AUTH_NONE = 0,  /* 本地 OpenAI 兼容端点（设计上无鉴权） */
    PROVIDER_AUTH_BEARER,    /* Authorization: Bearer <api_key> */
    PROVIDER_AUTH_X_API_KEY, /* x-api-key: <api_key>，anthropic 家族 */
} provider_auth_kind_t;

/* 请求头声明：auth 决定鉴权头样式，extra 为厂商静态附加头（形如
 * "anthropic-version: 2023-06-01"）。Content-Type: application/json 由 core
 * 统一附加，适配层无需重复声明。extra 指向静态生命周期字符串数组。 */
typedef struct {
    provider_auth_kind_t auth;
    const char *const *extra;
    size_t extra_count;
} provider_header_spec_t;

/* provider_name 用于 api_key_env 回落名推导（品牌无关统一约定，见
 * core/secrets.c sec_env_name_for）：未显式给出 "env:NAME" 时，以厂商名推
 * 出标准环境变量名供 secrets.env 热重载。 */
void provider_base_init(provider_base_ctx_t *base_ctx, const char *provider_name, const char *api_key,
                        const char *api_base, const char *organization, double timeout_sec,
                        int max_retries, const char *default_base);

/* 出网传输策略唯一实现（SSoT）：连接超时 / 总超时 / 代理 / 自定义 CA /
 * 重定向与证书校验。所有出网调用点（非流式、流式、google、anthropic）
 * 一律经此施加，禁止各自 curl_easy_setopt 副本。
 * timeout_sec 是整次 POST（含全部重试）的墙钟预算，不是单次尝试的预算。 */
void provider_http_setup(CURL *curl, double timeout_sec);

/* 出网失败分诊：把 libcurl 错误码归为可判读类别（DNS/CONNECT/TIMEOUT/TLS/
 * PROXY/NET_IO/OTHER），供日志与用户面诊断使用。返回值恒非 NULL。 */
const char *provider_http_diag(CURLcode code);

/* 出网重试策略唯一实现（SSoT，实现见 retry.c）：判断件与循环壳同处一件。
 *   retryable = provider_retryable(错误码)                  —— 是否值得重试
 *   delay     = provider_backoff_ms(已重试次数)             —— 指数退避 + 抖动
 *   left      = provider_left_sec(预算, 起始时刻)           —— 剩余墙钟秒数
 *   ok        = provider_retry_budget_ok(预算, 起点, delay) —— 退避后是否仍够一次
 * 只有四者同时成立才允许重试；流式还须满足"首包未下发"（provider_gate_fn）。
 * 任何一处复制这些判断都会造成同构点漂移。 */
#define PROVIDER_RETRY_BASE_MS 200U
#define PROVIDER_RETRY_MAX_MS 2000U
#define PROVIDER_RETRY_JITTER_PCT 25U
/* 退避等待之后仍须剩下的最小尝试窗口：低于此值就放弃重试，避免把调用方的
 * 墙钟预算空转在一次注定超时的尝试上。 */
#define PROVIDER_RETRY_MIN_LEFT_MS 1000U

int provider_retryable(CURLcode code);
uint32_t provider_backoff_ms(int attempt);
double provider_left_sec(double timeout_sec, uint64_t start_ms);
int provider_retry_budget_ok(double timeout_sec, uint64_t start_ms, uint32_t delay_ms);

/* 单次尝试的结局。 */
typedef enum {
    PROVIDER_TRY_FATAL = -1, /* 不可恢复（本地资源申请失败），立即终止整轮 */
    PROVIDER_TRY_FAILED = 0, /* 本次尝试失败，是否重试交循环壳判定 */
    PROVIDER_TRY_DONE = 1,   /* 本次尝试成立（传输层无错），立即终止整轮 */
} provider_attempt_t;

/* 单次尝试装配与执行（core/http.c 非流式、core/sse.c 流式各自实现）：绑定写
 * 回调、发起一次 curl_easy_perform 并回填 res / http_code。retry 为尝试序号
 * （0 起）；left_sec 为本次尝试可用的墙钟剩余预算，首试为全额 timeout_sec。 */
typedef provider_attempt_t (*provider_attempt_fn)(void *ud, int retry, double left_sec,
                                                  CURLcode *out_res, long *out_http_code);

/* 附加重试闸门（返回非 0 = 允许重试）：流式用它表达"上游已下发过字节即禁止
 * 重试"（重试会造成重复输出）。可为 NULL（表示无附加约束）。 */
typedef int (*provider_gate_fn)(void *ud, CURLcode res, long http_code);

/* 重试间复位钩子：清空上一次尝试的累积（响应缓冲 / SSE 分片状态）。可为 NULL。 */
typedef void (*provider_reset_fn)(void *ud);

typedef struct {
    const char *tag;    /* 日志标签（HTTP-POST / STREAM），恒非 NULL */
    const char *url;    /* 日志用请求 URL */
    double timeout_sec; /* 整轮重试的墙钟预算，非单次尝试预算 */
    int max_retries;    /* 额外重试次数（0 = 只尝试一次），负值按 0 处理 */
    provider_attempt_fn attempt;
    provider_gate_fn gate;   /* 可 NULL */
    provider_reset_fn reset; /* 可 NULL */
    void *ud;
} provider_retry_cfg_t;

/* 退避重试循环唯一实现（SSoT）。任一条件成立即终止循环：attempt 回
 * PROVIDER_TRY_DONE / PROVIDER_TRY_FATAL；已达 max_retries；gate 判定禁止
 * 重试或错误码不可重试；退避后剩余预算不足一次有效尝试。仅"可重试且预算
 * 充足"时才退避等待并调用 reset。
 * 返回 AIRY_OK —— 循环正常结束，成败由 out_res / out_http_code 判定；
 * 返回 AIRY_ERR_UNKNOWN —— attempt 报致命错误，out_* 未被改写。
 * 适配层禁止调用本件自建循环，只能经 provider_http_exec 出网。 */
int provider_retry_run(const provider_retry_cfg_t *cfg, CURLcode *out_res, long *out_http_code);

/* 传输原语（非流式）。仅 core 内部与 provider_http_exec 使用：url 与头链
 * 由调用方持有，适配层不得直连本原语（B16-S3.1——否则头链生命周期又会外溢
 * 到适配层，curl_* 亦随之回流）。 */
int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_http_resp_t **out_response,
                       long *out_http_code);

void provider_http_resp_free(provider_http_resp_t *resp);

char *provider_build_openai_request(const llm_request_config_t *manager, const char *default_model);

int provider_parse_openai_response(const char *body, llm_response_t **out);

/* 域内析构器：providers 失败路径释放的是自己解析的中间产物，析构归 provider
 * 域所有（B16-S2 内层反依赖——不依赖发布头 llm_service_free 门面）。语义与
 * rpc 域 llm_response_free 一致。 */
static inline void provider_response_free(llm_response_t *resp)
{
    if (!resp)
        return;
    AIRY_FREE(resp->id);
    AIRY_FREE(resp->model);
    AIRY_FREE(resp->finish_reason);
    if (resp->choices) {
        for (size_t i = 0; i < resp->choice_count; i++) {
            AIRY_FREE((void *)resp->choices[i].role);
            AIRY_FREE((void *)resp->choices[i].content);
            AIRY_FREE((void *)resp->choices[i].reasoning_content);
            AIRY_FREE((void *)resp->choices[i].tool_call_id);
            AIRY_FREE((void *)resp->choices[i].tool_calls_json);
        }
        AIRY_FREE(resp->choices);
    }
    AIRY_FREE(resp);
}

/* Grow-on-demand string append used by the streaming accumulators (content
 * and reasoning_content). Returns the (possibly reallocated) buffer; on
 * allocation failure returns NULL and leaves the input buffer untouched. */
char *provider_buf_append(char *buf, size_t *cap, size_t *len, const char *text);

typedef int (*provider_stream_chunk_cb_t)(const char *data_line, void *user_data);

/* 具名事件模式回调（SSE event:/data: 双行解析后交付；event 为 NULL 表示
 * 事件块无 event: 字段；data 恒 NUL 终结，data_len 为其字节长度）。 */
typedef int (*provider_sse_event_cb_t)(const char *event, const char *data, size_t data_len,
                                       void *user_data);

/* 流式 POST·行协议模式：SSE data 行载荷 NUL 终结后交付 on_chunk，
 * "[DONE]" 置正常终止。openai/deepseek/local 走此入口。
 * 重试边界：只有"上游一个字节都没下发"的失败才可重试——一旦写回调
 * 收到过数据，后续分片可能已经经 on_chunk 交付给用户，重试会造成重复输出，故
 * 此时无论错误是否瞬时都直接失败。HTTP 状态码不为 0 表示请求已到达上游，其重试
 * 策略归调用方（429/限流循环），此处不重试。 */
int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, int max_retries,
                              provider_stream_chunk_cb_t on_chunk, void *chunk_user_data,
                              long *out_http_code);

/* 流式 POST·具名事件模式：event:/data: 双行解析，事件名随载荷交付 on_event，
 * 空行复位事件块。anthropic 走此入口；重试边界与行协议模式一致，
 * 分帧/错误体诊断/重试循环与 provider_http_post_stream 共享唯一实现。
 * 与 provider_http_post 同属传输原语，适配层只经 provider_http_exec 到达。 */
int provider_http_post_stream_sse(const char *url, struct curl_slist *headers, const char *body,
                                  double timeout_sec, int max_retries,
                                  provider_sse_event_cb_t on_event, void *event_user_data,
                                  long *out_http_code);

/* 出网请求描述符（B16-S3.1）：唯一出网入口 provider_http_exec 的入参。适配层
 * 只声明"发给谁、发什么、怎么收"；URL 拼装、头链生命周期、限流与退避重试全部
 * 归 core——这是适配层零 I/O 的结构前提。 */
typedef struct {
    provider_base_ctx_t *base;        /* api_base / api_key / timeout_sec / max_retries */
    struct provider_rate_limiter *rl; /* 可 NULL；非流式且非 NULL 时经限流器出网 */
    const char *path;                 /* 追加在 api_base 之后，如 "/chat/completions" */
    const provider_header_spec_t *headers; /* 可 NULL = 无鉴权单 Content-Type */
    const char *body;
    provider_stream_chunk_cb_t on_chunk; /* 流式·行协议回调；非流式留 NULL */
    provider_sse_event_cb_t on_event;    /* 流式·具名事件回调；非流式留 NULL */
    void *user_data;                     /* 交付给 on_chunk / on_event */
} provider_request_t;

/* 唯一出网入口（B16-S3.1 SSoT）。按 on_event / on_chunk 是否为 NULL 分派具名
 * 事件流、行协议流或非流式；非流式且 rl 非 NULL 时叠加限流与 429/5xx 退避。
 * out_response 仅非流式模式写出（流式传 NULL）；out_http_code 恒非 NULL。
 * 返回值语义沿用各模式原入口：不限流的非流式返回 AIRY_OK 且成败看
 * out_http_code（4xx/5xx 响应体对调用方有诊断价值，不在此处吞掉）；限流模式
 * 与流式模式在 HTTP >= 400 时直接返回归一错误码。 */
int provider_http_exec(const provider_request_t *req, provider_http_resp_t **out_response,
                       long *out_http_code);

/* R-1 / N-3：provider HTTP 状态码 → airy 错误码归一（SSoT，实现见 http.c）。
 * 未命中已知状态码时返回 fallback（通常是传输层返回码）。 */
int provider_http_err_map(long http_code, int fallback);

/* 状态码 → 日志诊断串（与 err_map 配套）。返回值恒非 NULL。 */
const char *provider_http_err_diag(long http_code);

/* 流式控制帧发射（SSoT 唯一实现，收敛 openai/deepseek/local 的同构 static
 * 副本）。帧格式：工具帧 RS 'T' <json> RS；推理帧 RS 'R' <reasoning> RS。
 * RS(0x1E) 不出现在 cJSON 输出与 LLM 文本中，分帧无歧义；各段均 NUL 结尾
 * （llm_stream_callback 对 chunk 调 strlen()）。 */
void provider_emit_tool_frame(llm_stream_callback_t cb, void *ud, const char *tc_json);
void provider_emit_reasoning_frame(llm_stream_callback_t cb, void *ud, const char *reasoning);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_TRANSPORT_H */
