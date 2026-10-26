// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file prov_diag.c
 * @brief Provider 上游失败线程局域诊断槽（0.1.20 B1-d，P2-2 残余收口）。
 *
 * adapter/sse/rate_limit 各失败面本已握有上游状态码与响应体，但只落
 * daemon 日志：RPC 错误面仅剩 AIRY_ERR_* 一句映射，上游 400 根因要抓
 * 容器日志定位。本件打通透传通道：失败点释放 resp 前 set，methods.c
 * handler 失败出口读取拼接进 error.message。不另建错误体（ErrorModel
 * v1 铁律），不加全局锁（TLS 天然隔离）；complete/stream 服务调用同步
 * 运行于 RPC handler 线程，写点与读点必同线程，跨线程不透传即为设计
 * 边界。诊断仅到 message 字符串为止，与 JSONRPC_SEND_ERROR 无 data
 * 槽的现实一致；若 ErrorModel v1 err 信封全线接入落地，本槽文本原样
 * 迁入 diag 字段即可，接口不变。
 */

#include "prov_diag.h"

#include <stdio.h>
#include <string.h>

#include "platform_base.h"

#define PROV_DIAG_TEXT_MAX (PROV_DIAG_BODY_MAX + 64)

static AIRY_THREAD_LOCAL char g_prov_diag[PROV_DIAG_TEXT_MAX];

void prov_diag_set(long http_code, const char *body, size_t body_len)
{
    if (body && body_len > 0) {
        if (body_len > PROV_DIAG_BODY_MAX)
            body_len = PROV_DIAG_BODY_MAX;
        if (http_code > 0)
            snprintf(g_prov_diag, sizeof(g_prov_diag), "upstream http %ld: %.*s", http_code,
                     (int)body_len, body);
        else
            snprintf(g_prov_diag, sizeof(g_prov_diag), "upstream transport: %.*s", (int)body_len,
                     body);
    } else if (http_code > 0) {
        snprintf(g_prov_diag, sizeof(g_prov_diag), "upstream http %ld (empty body)", http_code);
    } else {
        prov_diag_clear();
    }
}

void prov_diag_clear(void)
{
    g_prov_diag[0] = '\0';
}

const char *prov_diag_text(void)
{
    return g_prov_diag;
}
