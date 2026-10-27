/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_retry.h: Provider 域退避重试策略（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_RETRY_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_RETRY_H

#include "provider_retry_class.h"

#include <curl/curl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROVIDER_RETRY_BASE_MS 200U
#define PROVIDER_RETRY_MAX_MS 2000U
#define PROVIDER_RETRY_JITTER_PCT 25U
#define PROVIDER_RETRY_MIN_LEFT_MS 1000U /* 退避后仍须剩下的最小尝试窗口 */
#define PROVIDER_RETRY_LOCAL_MS 500U     /* 本地端点重连：固定短退避 */

int provider_retryable(CURLcode code);
uint32_t provider_backoff_ms(int attempt);
double provider_left_sec(double timeout_sec, uint64_t start_ms);
int provider_retry_budget_ok(double timeout_sec, uint64_t start_ms, uint32_t delay_ms);

typedef enum {
    PROVIDER_TRY_FATAL = -1, /* 不可恢复，立即终止整轮 */
    PROVIDER_TRY_FAILED = 0, /* 本次失败，是否重试交循环壳判定 */
    PROVIDER_TRY_DONE = 1,   /* 本次成立（传输层无错），立即终止整轮 */
} provider_attempt_t;

typedef provider_attempt_t (*provider_attempt_fn)(void *ud, int retry, double left_sec,
                                                  CURLcode *out_res, long *out_http_code,
                                                  int *out_tmo);

/* 附加重试闸门（非 0 = 允许重试）；流式用它表达"已下发字节即禁止重试"。可 NULL。 */
typedef int (*provider_gate_fn)(void *ud, CURLcode res, long http_code);

typedef void (*provider_reset_fn)(void *ud);

typedef struct {
    const char *tag;    /* 日志标签（HTTP-POST / STREAM），恒非 NULL */
    const char *url;    /* 日志用请求 URL */
    double timeout_sec; /* 整轮重试的墙钟预算，非单次尝试预算 */
    int max_retries;    /* 额外重试次数（0 = 只尝试一次），负值按 0 处理 */
    provider_retry_class_t retry_class; /* 可重试类别收窄（策略面），缺省 DEFAULT */
    int idempotent;          /* 1 = 重复投递语义等价；决定"可能已送达"的故障可否重发 */
    provider_attempt_fn attempt;
    provider_gate_fn gate;   /* 可 NULL */
    provider_reset_fn reset; /* 可 NULL */
    void *ud;
} provider_retry_cfg_t;

/* AIRY_OK = 循环正常结束（成败由 out_res/out_http_code 判定）；AIRY_ERR_UNKNOWN = attempt 报致命错误。
 * out_tmo 可 NULL，非 NULL 时回填最后一次尝试的超时归类（prov_tmo_t）。 */
int provider_retry_run(const provider_retry_cfg_t *cfg, CURLcode *out_res, long *out_http_code,
                       int *out_tmo);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_RETRY_H */
