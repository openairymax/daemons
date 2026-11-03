/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* prov_diag.h: Provider 上游失败线程局域诊断槽（0.1.20 B1-d，P2-2 残余收口） */

#ifndef LLM_D_PROVIDERS_CORE_PROV_DIAG_H
#define LLM_D_PROVIDERS_CORE_PROV_DIAG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 上游响应体进入诊断文本的截断上界（字节）。 */
#define PROV_DIAG_BODY_MAX 512

/*
 * 记录本线程最近一次 provider 上游失败：http_code > 0 时格式化为
 * "upstream http <code>: <body 片段>"，== 0 视为传输层失败
 * （"upstream transport: <body 片段>"）；body 为 NULL 或零长且
 * code <= 0 时清空。后写覆盖前写（重试语义 = 上报最后一次失败）。
 */
void prov_diag_set(long http_code, const char *body, size_t body_len);

/* 清空本线程诊断槽。RPC handler 发起服务调用前调用一次，防止线程复用
 * 时上一个请求的陈旧诊断污染本次错误面。 */
void prov_diag_clear(void);

/* 本线程诊断文本；未设置时返回 ""。指针指向线程局域存储，仅在同线程
 * 下一次 prov_diag_set/clear 前有效。 */
const char *prov_diag_text(void);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROV_DIAG_H */
