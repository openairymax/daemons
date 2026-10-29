// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_rpc_neg.c
 * @brief Daemon RPC 可达性负缓存（0.1.20 D2-1/D2-4）
 *
 * 从 daemon_rpc_client.c 拆出（公共 API/ABI 不变，逻辑逐字保留）：缓存
 * 的不是连接，而是「不可达事实」。连接复用在本协议下不可实现——服务端
 * daemon_handle_client 严格「单请求单响应即关闭」，缓存的 fd 在响应返回
 * 后已被对端关闭；因此这里只登记「多久之内不必再试」，避免每轮重复
 * connect 与错误日志刷屏。
 *
 * 只有「曾经连通过」的目标（seen=1）才允许进入退避：daemon 首次启动前
 * 尚未建 socket 属启动竞态，若也计入退避，会把竞态放大为最长 30s 的假
 * 不可达（稳定性回归）。退避梯度 1s→2s→4s→8s→16s→30s（封顶），连接
 * 成功即清零。表容量 8 覆盖 daemon 目标数；满则不再登记新目标。
 */

#include "daemon_rpc_neg_internal.h"

#include "platform_misc.h"
#include "platform_sync.h"

#include <string.h>

#define RPC_NEG_MAX 8
#define RPC_NEG_NAME_MAX 128
#define RPC_NEG_BASE_MS 1000u
#define RPC_NEG_LEVEL_MAX 6u
#define RPC_NEG_CAP_MS 30000u

typedef struct {
    char target[RPC_NEG_NAME_MAX];
    uint64_t until_ms;
    uint8_t level;
    uint8_t seen;
} rpc_neg_t;

static rpc_neg_t g_neg[RPC_NEG_MAX];
static airy_mtx_t g_neg_mtx;
static airy_once_t g_neg_once = AIRY_ONCE_INIT;
static int g_neg_ready;

static void neg_init(void)
{
    g_neg_ready = (airy_mtx_init(&g_neg_mtx) == 0);
}

static void neg_lock(void)
{
    airy_call_once(&g_neg_once, neg_init);
    if (g_neg_ready)
        (void)airy_mtx_lock(&g_neg_mtx);
}

static void neg_unlock(void)
{
    if (g_neg_ready)
        (void)airy_mtx_unlock(&g_neg_mtx);
}

static void neg_name(char *dst, const char *src)
{
    size_t i = 0;
    if (src) {
        for (; i + 1 < RPC_NEG_NAME_MAX && src[i]; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

static rpc_neg_t *neg_find(const char *target)
{
    int i;
    for (i = 0; i < RPC_NEG_MAX; i++) {
        if (g_neg[i].target[0] && strcmp(g_neg[i].target, target) == 0)
            return &g_neg[i];
    }
    return NULL;
}

/* 成功路径不淘汰已有条目：容量满即放弃登记，避免把在退避的目标挤掉。 */
static rpc_neg_t *neg_grow(const char *target)
{
    rpc_neg_t *slot = neg_find(target);
    int i;

    if (slot)
        return slot;
    for (i = 0; i < RPC_NEG_MAX; i++) {
        if (!g_neg[i].target[0]) {
            slot = &g_neg[i];
            break;
        }
    }
    if (!slot)
        return NULL;
    neg_name(slot->target, target);
    slot->until_ms = 0;
    slot->level = 0;
    slot->seen = 0;
    return slot;
}

static uint64_t neg_delay(uint8_t level)
{
    uint64_t ms = RPC_NEG_BASE_MS;
    uint8_t i;

    for (i = 1; i < level; i++)
        ms <<= 1;
    return ms < RPC_NEG_CAP_MS ? ms : RPC_NEG_CAP_MS;
}

int rpc_neg_blocked(const char *target)
{
    int blocked = 0;

    if (!target)
        return 0;
    neg_lock();
    if (g_neg_ready) {
        const rpc_neg_t *e = neg_find(target);
        blocked = (e && e->until_ms > airy_time_ms());
    }
    neg_unlock();
    return blocked;
}

void rpc_neg_fail(const char *target)
{
    if (!target)
        return;
    neg_lock();
    if (g_neg_ready) {
        rpc_neg_t *e = neg_find(target);
        if (e && e->seen) {
            if (e->level < RPC_NEG_LEVEL_MAX)
                e->level++;
            e->until_ms = airy_time_ms() + neg_delay(e->level);
        }
    }
    neg_unlock();
}

void rpc_neg_ok(const char *target)
{
    if (!target)
        return;
    neg_lock();
    if (g_neg_ready) {
        rpc_neg_t *e = neg_grow(target);
        if (e) {
            e->seen = 1;
            e->level = 0;
            e->until_ms = 0;
        }
    }
    neg_unlock();
}
