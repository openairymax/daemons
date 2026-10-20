/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_failover.c
 * @brief 端点切换决策件实现（0.1.19 §3.8.2）。
 *
 * 两级判据：先查显式表（class 归类无法表达的三类），再按 ErrorModel
 * class 兜底。显式表必须前置，否则 AIRY_ERR_IO 会被 business 吞掉、
 * AIRY_ERR_LLM_AUTH_FAIL 会被 config 吞掉。
 */

#include "llm_failover.h"

#include "err_class.h"
#include "error_codes.h"

#include <string.h>

typedef struct {
    int code;
    llm_fo_reason_t reason;
} fo_map_t;

static const fo_map_t k_fo_map[] = {
    {AIRY_ERR_LLM_AUTH_FAIL, LLM_FO_AUTH},
    {AIRY_ERR_LLM_RATE_LIMIT, LLM_FO_RATE_LIMIT},
    {AIRY_ERR_IO, LLM_FO_TRANSPORT},
};

static llm_fo_reason_t fo_map_find(int code)
{
    size_t i;

    for (i = 0; i < sizeof(k_fo_map) / sizeof(k_fo_map[0]); ++i) {
        if (k_fo_map[i].code == code)
            return k_fo_map[i].reason;
    }
    return LLM_FO_NONE;
}

llm_fo_reason_t llm_fo_reason(int err_code)
{
    int code = err_code > 0 ? -err_code : err_code;
    llm_fo_reason_t mapped;

    if (code == 0)
        return LLM_FO_NONE;

    mapped = fo_map_find(code);
    if (mapped != LLM_FO_NONE)
        return mapped;

    /* 链路层故障（超时/连接重置/EOF/服务未就绪/供给缺失）换端点有意义。 */
    if (airy_err_class_of(code) == AIRY_ERR_CLASS_TRANSPORT)
        return LLM_FO_TRANSPORT;

    return LLM_FO_NONE;
}

int llm_fo_should(int err_code, unsigned on_mask)
{
    llm_fo_reason_t reason = llm_fo_reason(err_code);

    return reason != LLM_FO_NONE && (on_mask & LLM_FO_BIT(reason)) != 0;
}

const char *llm_fo_name(llm_fo_reason_t reason)
{
    static const char *const names[] = {"none", "transport", "rate_limit", "auth"};

    if ((int)reason < 0 || (int)reason >= (int)(sizeof(names) / sizeof(names[0])))
        return "none";
    return names[(int)reason];
}

llm_fo_reason_t llm_fo_parse(const char *name)
{
    int r;

    if (name == NULL)
        return LLM_FO_NONE;

    for (r = 0; r <= (int)LLM_FO_AUTH; ++r) {
        if (strcmp(name, llm_fo_name((llm_fo_reason_t)r)) == 0)
            return (llm_fo_reason_t)r;
    }
    return LLM_FO_NONE;
}
