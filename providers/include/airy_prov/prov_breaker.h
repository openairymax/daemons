/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file prov_breaker.h
 * @brief Provider 域调用侧熔断机制件契约（0.1.21 B-4 / C-3，SSoT）。
 *
 * 与 llm_d 路由层端点健康的分工：端点健康回答「可达性与冷加载」，带探针
 * 状态；本件回答「调用侧连续失败到何种程度必须快速失败」，按 provider 名
 * 聚合。二者共用同一事实来源（调用结果），本件只决定快速失败闸，不参与
 * 路由评分，不持有第二套阈值语义。
 *
 * 三态与 commons cb_*（utils/ipc/circuit_breaker.h）同构：CLOSED / OPEN /
 * HALF_OPEN；冷却结束自动半开，半开放行探针，探针成功即闭合。fail_max=0
 * 表示关闭闸门：allow 恒放行，ok/fail/tmo 只记账，现网行为逐字节不变。
 *
 * C-3 口径：
 *   1. 超时先分类——端点慢（CONNECT/STALL）计入连败；预算不够（TOTAL）
 *      只计预算口径，不推进连败（预算不足不是端点故障）。
 *   2. circuit_breaks 与「被拒次数」同值：熔断拒绝与预算拒绝共用该口径，
 *      不另立计数器；CLI/GRAD 只读这一个数。
 *   3. GRAD 的判定输入由 prov_brk_gate() 单点给出（失败计数、冷却窗口、
 *      预算不足事件与 breaks），调用方不必解析 JSON、不得到表里翻账。
 */

#ifndef LLM_D_PROVIDERS_CORE_PROV_BREAKER_H
#define LLM_D_PROVIDERS_CORE_PROV_BREAKER_H

#include "airy_prov/prov_stat.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROV_BRK_MAX 64
#define PROV_BRK_NAME_MAX 64
#define PROV_BRK_DEF_FAIL_MAX 5u
#define PROV_BRK_DEF_OPEN_MS 30000u
#define PROV_BRK_DEF_HALF_MAX 1u

/* 读数态：前三个与 cb_state_t 对齐，NONE 表示尚无调用记录。 */
typedef enum {
    PROV_BRK_CLOSED = 0,
    PROV_BRK_OPEN = 1,
    PROV_BRK_HALF = 2,
    PROV_BRK_NONE = 3
} prov_brk_t;

typedef struct {
    uint32_t fail_max; /* 连续失败阈值；0 = 关闭闸门（记账仍生效） */
    uint32_t open_ms;  /* 打开后的冷却窗口 */
    uint32_t half_max; /* 半开期允许的连续探针上限（每次放行一个） */
} prov_brk_cfg_t;

typedef struct {
    uint32_t state;      /* prov_brk_t */
    uint64_t calls;      /* 放行次数 */
    uint64_t oks;        /* 成功次数 */
    uint64_t fails;      /* 端点故障（传输/5xx/鉴权等） */
    uint64_t slow_tmo;   /* 端点慢超时（计入连败） */
    uint64_t budget_tmo; /* 预算不够（不计连败） */
    uint64_t rejects;    /* 被闸拒绝次数 */
    uint64_t opens;      /* 进入 OPEN 的次数 */
    uint64_t breaks;     /* = rejects + budget_tmo：CLI/GRAD 共用口径 */
} prov_brk_stat_t;

/* GRAD/CLI 的判定输入（C-3）：单个 provider 的快速失败视图。
 * 失败计数与预算不足分列，冷却窗口给出剩余毫秒——策略层据此决定降级、
 * 收敛轮次或直接快速失败，不需要再读 JSON 或推算时间戳。 */
typedef struct {
    uint32_t state;        /* prov_brk_t */
    uint32_t open_left_ms; /* OPEN 剩余冷却；HALF/CLOSED/NONE 为 0 */
    uint64_t fails;        /* 端点失败累计（fails + slow_tmo） */
    uint64_t budget_tmo;   /* 预算不足终止次数 */
    uint64_t breaks;       /* 被拒累计（熔断拒绝 + 预算终止） */
} prov_brk_gate_t;

/* 缺省配置（fail_max=5 / open_ms=30s / half_max=1），NULL 安全。 */
void prov_brk_def(prov_brk_cfg_t *cfg);

/* 进程级配置；缺省值可由环境变量覆盖：
 * AIRY_PROV_BRK_FAIL_MAX（0 = 关闭）、AIRY_PROV_BRK_OPEN_MS、
 * AIRY_PROV_BRK_HALF_MAX。cfg 为 NULL 时按环境/缺省重建。 */
int prov_brk_cfg(const prov_brk_cfg_t *cfg);

/* 闸门：1 放行 / 0 拒绝；拒绝即 breaks++。provider 为空按放行处理
 * （无身份不得阻断调用）。 */
int prov_brk_allow(const char *provider);

/* 调用结果记账（与 allow 成对）。err 为 AIRY_ERR_*；记录未知 provider
 * 时按新记录建账，providers 上限满时按放行处理、不记账。 */
void prov_brk_ok(const char *provider);
void prov_brk_fail(const char *provider, int err);
void prov_brk_tmo(const char *provider, prov_tmo_t kind);

/* 读面：state 返回 prov_brk_t；stat 未命中返回 -1、命中 0。 */
int prov_brk_state(const char *provider);
int prov_brk_stat(const char *provider, prov_brk_stat_t *out);

/* 判定输入快照：0 命中 / -1 无记录（只读，不建账、不改状态）。 */
int prov_brk_gate(const char *provider, prov_brk_gate_t *out);

/* 全量快照（JSON 数组，调用方 AIRY_FREE）；无记录返回 "[]"。 */
char *prov_brk_json(void);

/* 全 provider 累计被拒次数（CLI/GRAD 的 circuit_breaks 唯一读数）。 */
uint64_t prov_brk_breaks(void);

/* 清空记录与计数，配置保持不变（测试与热重载用）。 */
void prov_brk_reset(void);

/* 状态名：closed / open / half_open / none。 */
const char *prov_brk_text(uint32_t state);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROV_BREAKER_H */
