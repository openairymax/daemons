/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_sub_policy.c
 * @brief 子任务委派策略面实现：配额解析、线程内深度、并发槽位与工具契约。
 */

#include "agent_sub_policy.h"

#include <stdlib.h>

#include "platform.h"

/* 部署配置注入值：0 表示未配置（回落环境变量/缺省）。 */
static airy_atomic_int_t g_lim_depth;
static airy_atomic_int_t g_lim_conc;
/* 在途子任务数：进入/退出都是短临界区，原子自增即可，无需互斥。 */
static airy_atomic_int_t g_live;

/* 深度属于调用链：agent.run 每请求一线程，委派在同一线程内同步下探，
 * 线程局部计数与链路的实际层数一一对应；换成进程级计数会把并行会话
 * 的深度相加，误伤第二条链。 */
static AIRY_THREAD_LOCAL int g_depth = 0;

static int env_clamp(const char *name, int def, int cap)
{
    const char *raw = getenv(name);
    if (!raw || !raw[0])
        return def;
    char *end = NULL;
    long v = strtol(raw, &end, 10);
    if (end == raw || v <= 0)
        return def;
    return v > cap ? cap : (int)v;
}

void sub_limits_set(int max_depth, int max_conc)
{
    if (max_depth > AGENT_SUB_DEPTH_CAP)
        max_depth = AGENT_SUB_DEPTH_CAP;
    if (max_conc > AGENT_SUB_CONC_CAP)
        max_conc = AGENT_SUB_CONC_CAP;
    airy_atomic_store(&g_lim_depth, max_depth > 0 ? max_depth : 0);
    airy_atomic_store(&g_lim_conc, max_conc > 0 ? max_conc : 0);
}

agent_sub_limits_t sub_limits(void)
{
    agent_sub_limits_t lim;
    lim.max_depth = airy_atomic_load(&g_lim_depth);
    lim.max_conc = airy_atomic_load(&g_lim_conc);
    if (lim.max_depth <= 0)
        lim.max_depth = env_clamp(AGENT_SUB_ENV_DEPTH, AGENT_SUB_DEPTH_DEFAULT, AGENT_SUB_DEPTH_CAP);
    if (lim.max_conc <= 0)
        lim.max_conc = env_clamp(AGENT_SUB_ENV_CONC, AGENT_SUB_CONC_DEFAULT, AGENT_SUB_CONC_CAP);
    return lim;
}

int sub_depth(void)
{
    return g_depth;
}

int sub_enter(void)
{
    if (g_depth >= sub_limits().max_depth)
        return 1;
    g_depth++;
    return 0;
}

void sub_leave(void)
{
    if (g_depth > 0)
        g_depth--;
}

int sub_slot_try(void)
{
    int cap = sub_limits().max_conc;
    if (airy_atomic_fetch_add(&g_live, 1) >= cap) {
        airy_atomic_fetch_sub(&g_live, 1);
        return 0;
    }
    return 1;
}

void sub_slot_drop(void)
{
    airy_atomic_fetch_sub(&g_live, 1);
}

const char *sub_desc(void)
{
    return "把可分离的子目标委派给独立子代理：子代理有独立上下文与工具循环，"
           "只回传结论与关键证据。仅当子目标自洽、不需要父会话逐字历史时使用。";
}

const char *sub_schema(void)
{
    return "{\"type\":\"object\",\"properties\":{"
           "\"goal\":{\"type\":\"string\",\"description\":\"子任务目标与交付物\"},"
           "\"context\":{\"type\":\"string\",\"description\":\"子任务所需的全部背景；子代理看不到父会话历史\"},"
           "\"budget\":{\"type\":\"integer\",\"description\":\"子任务 token 上限，缺省取父预算的三分之一\"}},"
           "\"required\":[\"goal\"]}";
}

const char *sub_lead(void)
{
    return "你是父任务委派出来的子代理。只围绕下述子目标独立工作，最后回传结论与关键证据，"
           "不要复述过程。父会话历史对你不可见，所需背景以本提示中的上下文为准。\n\n";
}
