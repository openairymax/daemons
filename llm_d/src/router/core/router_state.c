/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file router_state.c
 * @brief 端点健康面的落盘与恢复实现（0.1.20 D2-3③）。
 *
 * 文件为制表符分隔的纯文本，落在 $AIRY_DATA_DIR/router.state，与
 * llm_usage.json 同域。写入走 airy_io_write_file() 的同目录临时文件 +
 * 原子改名，崩溃不会留下半行状态。格式按行自描述，版本演进只增列，
 * 少列的行整行丢弃，不猜测。
 */

#include "router/core/router_state.h"

#include "airy_memory.h"
#include "error_codes.h"
#include "io.h"
#include "platform_misc.h"
#include "platform_paths.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RTR_STATE_NAME "router.state"
#define RTR_STATE_LINE_MAX 320U

/* 路径每次现算：空中台允许测试在同一进程内切换 AIRY_HOME。 */
static int state_path(char *out, size_t cap)
{
    const char *dir = airy_data_dir();

    if (!dir || !dir[0])
        return -1;
    if (snprintf(out, cap, "%s/%s", dir, RTR_STATE_NAME) >= (int)cap)
        return -1;
    return 0;
}

/* 端点健康面 → 快照。保存与热重载前捕获共用同一换算口径。 */
static void snap_from_ep(const llm_endpoint_t *ep, uint32_t now, router_ep_snap_t *s)
{
    llm_ep_state_t st = llm_ep_state(ep, now);

    AIRY_STRNCPY_TERM(s->provider_name, ep->provider_name, sizeof(s->provider_name));
    AIRY_STRNCPY_TERM(s->model_name, ep->model_name, sizeof(s->model_name));
    s->consec_fail = ep->consec_fail;
    s->last_fail_age_ms = ep->last_fail_ms != 0U ? now - ep->last_fail_ms : 0U;
    /* 失败当拍保存会得到 0，恢复时被当作"从未失败"而提前衰减清零，
     * 故有失败计数时把年龄抬到 1ms。 */
    if (ep->consec_fail != 0U && s->last_fail_age_ms == 0U)
        s->last_fail_age_ms = 1U;
    s->open_remain_ms =
        st == LLM_EP_OPEN ? ep->open_until_ms - now : 0U;
    s->probe_fail = ep->probe_fail;
    s->open_level = ep->open_level;
    s->state = (uint8_t)st;
}

/* 十进制无符号解析：只收纯数字，空串与溢出即失败。合规规范禁用 sscanf，
 * 此处分列手解，保证解析口径与写入格式严格对称。 */
static int field_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0U;

    if (!s || !*s)
        return -1;
    for (; *s; s++) {
        unsigned d;

        if (*s < '0' || *s > '9')
            return -1;
        d = (unsigned)(*s - '0');
        if (v > (UINT32_MAX - d) / 10U)
            return -1;
        v = v * 10U + d;
    }
    *out = v;
    return 0;
}

static void state_parse(char *line)
{
    router_ctx_t *ctx = router_ctx_get();
    router_ep_snap_t *s;
    char *f[8];
    char *p = line;
    size_t n = 0U;
    uint32_t fail = 0U;
    uint32_t age = 0U;
    uint32_t state = 0U;
    uint32_t remain = 0U;
    uint32_t probe = 0U;
    uint32_t level = 0U;

    while (n < 8U) {
        char *tab = strchr(p, '\t');

        f[n++] = p;
        if (!tab)
            break;
        *tab = '\0';
        p = tab + 1;
    }
    if (n != 8U || strchr(f[7], '\t') != NULL)
        return;
    if (!f[0][0] || !f[1][0])
        return;
    if (field_u32(f[2], &fail) != 0 || field_u32(f[3], &age) != 0 ||
        field_u32(f[4], &state) != 0 || field_u32(f[5], &remain) != 0 ||
        field_u32(f[6], &probe) != 0 || field_u32(f[7], &level) != 0)
        return;
    if (state > (uint32_t)LLM_EP_HALF)
        return;
    if (ctx->restore_count >= LLM_ROUTER_MAX_ENDPOINTS)
        return;

    s = &ctx->restore[ctx->restore_count++];
    AIRY_STRNCPY_TERM(s->provider_name, f[0], sizeof(s->provider_name));
    AIRY_STRNCPY_TERM(s->model_name, f[1], sizeof(s->model_name));
    s->consec_fail = fail > LLM_EP_FAIL_MAX ? LLM_EP_FAIL_MAX : fail;
    s->last_fail_age_ms = age > LLM_EP_FAIL_WINDOW_MS ? LLM_EP_FAIL_WINDOW_MS : age;
    s->open_remain_ms = remain > LLM_EP_OPEN_MAX_MS ? LLM_EP_OPEN_MAX_MS : remain;
    s->probe_fail = probe > LLM_EP_PROBE_MAX ? LLM_EP_PROBE_MAX : probe;
    s->open_level = level > LLM_EP_MAX_LEVEL ? (uint8_t)LLM_EP_MAX_LEVEL : (uint8_t)level;
    s->state = (uint8_t)state;
}

void router_state_load(void)
{
    router_ctx_t *ctx = router_ctx_get();
    char path[AIRY_PATH_MAX];
    char *buf = NULL;
    size_t len = 0;
    char *line;

    ctx->restore_count = 0U;
    if (state_path(path, sizeof(path)) != 0)
        return;
    if (airy_io_read_file(path, &buf, &len) != 0)
        return; /* 首次运行无文件：常态，不是错误 */
    if (len == 0U) {
        AIRY_FREE(buf);
        return;
    }

    line = buf;
    while (line && *line && ctx->restore_count < LLM_ROUTER_MAX_ENDPOINTS) {
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = '\0';
        state_parse(line);
        line = nl ? nl + 1 : NULL;
    }
    AIRY_FREE(buf);

    if (ctx->restore_count != 0U)
        AIRY_LOG_INFO("C-L02: LLMRouter: restored health for %zu endpoint(s)",
                      ctx->restore_count);
}

void rtr_state_capture(void)
{
    router_ctx_t *ctx = router_ctx_get();
    uint32_t now = (uint32_t)airy_time_ms();
    size_t i;

    ctx->restore_count = 0U;
    for (i = 0; i < ctx->endpoint_count && i < LLM_ROUTER_MAX_ENDPOINTS; i++)
        snap_from_ep(&ctx->endpoints[i], now, &ctx->restore[ctx->restore_count++]);
}

void rtr_ep_restore(llm_endpoint_t *ep)
{
    router_ctx_t *ctx = router_ctx_get();
    uint32_t now = (uint32_t)airy_time_ms();
    size_t i;

    for (i = 0; i < ctx->restore_count; i++) {
        router_ep_snap_t *s = &ctx->restore[i];

        if (strcmp(s->provider_name, ep->provider_name) != 0 ||
            strcmp(s->model_name, ep->model_name) != 0)
            continue;

        ep->consec_fail = s->consec_fail;
        ep->probe_fail = s->probe_fail;
        ep->open_level = s->open_level;
        ep->last_fail_ms = s->last_fail_age_ms != 0U ? now - s->last_fail_age_ms : 0U;
        ep->probe_until_ms = 0U;
        if (s->state == (uint8_t)LLM_EP_OPEN)
            ep->open_until_ms = now + (s->open_remain_ms != 0U ? s->open_remain_ms : 1U);
        else if (s->state == (uint8_t)LLM_EP_HALF)
            ep->open_until_ms = now != 0U ? now : 1U;
        else
            ep->open_until_ms = 0U;

        /* 用过即摘除：同名端点重复注册不应再吃同一份旧状态。 */
        ctx->restore[i] = ctx->restore[ctx->restore_count - 1U];
        ctx->restore_count--;
        return;
    }
}

int rtr_state_save(void)
{
    router_ctx_t *ctx = router_ctx_get();
    char path[AIRY_PATH_MAX];
    uint32_t now = (uint32_t)airy_time_ms();
    char *buf;
    size_t cap;
    size_t pos = 0U;
    size_t i;
    int rc;

    if (!ctx->initialized)
        return AIRY_ERR_SYS_NOT_INIT;
    if (ctx->endpoint_count == 0U)
        return AIRY_SUCCESS;
    if (state_path(path, sizeof(path)) != 0)
        return AIRY_ERR_INVALID_PARAM;

    cap = ctx->endpoint_count * (RTR_STATE_LINE_MAX + 1U) + 1U;
    buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;
    buf[0] = '\0';

    for (i = 0; i < ctx->endpoint_count; i++) {
        router_ep_snap_t s;
        int n;

        snap_from_ep(&ctx->endpoints[i], now, &s);
        n = snprintf(buf + pos, cap - pos, "%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\n", s.provider_name,
                     s.model_name, (unsigned)s.consec_fail, (unsigned)s.last_fail_age_ms,
                     (unsigned)s.state, (unsigned)s.open_remain_ms, (unsigned)s.probe_fail,
                     (unsigned)s.open_level);
        if (n <= 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }

    /* 路径惰性解析不创建目录，首启或测试隔离目录需在此补齐。 */
    (void)airy_io_mkdir_p(airy_data_dir(), 0700);
    rc = airy_io_write_file(path, buf, pos);
    AIRY_FREE(buf);
    return rc == 0 ? AIRY_SUCCESS : AIRY_ERR_IO;
}
