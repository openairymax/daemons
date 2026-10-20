/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service_route.c
 * @brief model.yaml routing 段解析（0.1.19 §3.8.4 A4）。
 *
 * 配置面只回答「怎么切」，机制件 service_chain 只回答「还能不能切」。
 * 本件读取到现有 llm_route_cfg_t 即止，不持有请求状态、不触碰注册表，
 * 使 A3 的切换循环对配置来源完全无感。
 *
 * 段内键与缺省语义：
 *   routing.mode          off（默认，现网零变化）| auto；未知值告警后按 off
 *   routing.chain_depth   最多切换跳数，缺省 2，限幅 [0, SVC_CHAIN_MAX_TRIED]
 *   routing.on            可切换类序列，逐项映射 LLM_FO_BIT()；缺失保留缺省，
 *                         显式空表表示禁止一切切换
 *   routing.deadline_sec  单次 llm.call 总预算（秒），×1000 落入 budget_ms；
 *                         <=0 表示不限
 *
 * 解析件对「未声明」与「显式声明」严格区分：未声明不覆盖，使主配置与
 * 用户覆盖可按顺序叠加，用户只覆盖自己写下的键。
 */

#include "rpc/service_chain.h"

#include "error.h"
#include "router/core/llm_failover.h"
#include "svc_logger.h"
#include "yaml_minimal.h"

#include <ctype.h>
#include <limits.h>

/* 大小写不敏感的整串比较：配置面取值按惯例小写，但不惩罚大写写法。 */
static int route_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void route_mode(struct yaml_node *rt, llm_route_cfg_t *cfg)
{
    const char *val = yaml_as_string(yaml_get(rt, "mode"), NULL);

    if (!val)
        return;
    if (route_ieq(val, "auto")) {
        cfg->mode = SVC_CHAIN_AUTO;
        return;
    }
    if (!route_ieq(val, "off"))
        SVC_LOG_WARN("C-L02: SVC: ROUTE unknown mode '%s', using off", val);
    cfg->mode = SVC_CHAIN_OFF;
}

static void route_depth(struct yaml_node *rt, llm_route_cfg_t *cfg)
{
    struct yaml_node *node = yaml_get(rt, "chain_depth");
    long long val;

    if (!node)
        return;
    val = yaml_as_int64(node, -1);
    if (val < 0) {
        SVC_LOG_WARN("C-L02: SVC: ROUTE chain_depth %lld clamped to 0", val);
        val = 0;
    }
    if (val > SVC_CHAIN_MAX_TRIED)
        val = SVC_CHAIN_MAX_TRIED;
    cfg->depth = (int)val;
}

static void route_on(struct yaml_node *rt, llm_route_cfg_t *cfg)
{
    struct yaml_node *seq = yaml_get(rt, "on");
    size_t count;
    unsigned mask = 0;

    if (!seq)
        return;
    if (seq->type != YAML_NODE_SEQUENCE) {
        SVC_LOG_WARN("C-L02: SVC: ROUTE 'on' is not a sequence, keeping defaults");
        return;
    }

    count = yaml_size(seq);
    for (size_t i = 0; i < count; i++) {
        const char *name = yaml_as_string(yaml_get_index(seq, i), NULL);
        llm_fo_reason_t reason = llm_fo_parse(name);

        if (reason == LLM_FO_NONE) {
            SVC_LOG_WARN("C-L02: SVC: ROUTE unknown on class '%s' skipped",
                         name ? name : "(null)");
            continue;
        }
        mask |= LLM_FO_BIT(reason);
    }
    cfg->on_mask = mask;
}

static void route_deadline(struct yaml_node *rt, llm_route_cfg_t *cfg)
{
    struct yaml_node *node = yaml_get(rt, "deadline_sec");
    long long sec;

    if (!node)
        return;
    sec = yaml_as_int64(node, -1);
    if (sec <= 0) {
        cfg->budget_ms = 0;
        return;
    }
    /* 秒 → 毫秒不得溢出 int；限幅后最坏 ≈24.8 天，远超任何单次调用预算。 */
    if (sec > INT_MAX / 1000)
        sec = INT_MAX / 1000;
    cfg->budget_ms = (int)(sec * 1000);
}

int svc_route_load(const char *path, llm_route_cfg_t *cfg)
{
    yaml_document_t *doc;
    struct yaml_node *root;
    struct yaml_node *rt;

    if (!path || !cfg)
        return AIRY_ERR_INVALID_PARAM;

    doc = yaml_create();
    if (!doc)
        return AIRY_ERR_UNKNOWN;
    if (yaml_parse_file(doc, path) != 0) {
        const char *msg = yaml_get_error(doc);
        SVC_LOG_WARN("C-L02: SVC: ROUTE parse %s failed: %s", path, msg ? msg : "?");
        yaml_destroy(doc);
        return AIRY_ERR_IO;
    }

    root = yaml_root(doc);
    rt = yaml_get(root, "routing");
    if (!rt) {
        yaml_destroy(doc);
        return AIRY_ERR_NOT_FOUND;
    }
    if (rt->type != YAML_NODE_MAPPING) {
        SVC_LOG_WARN("C-L02: SVC: ROUTE section in %s is not a mapping", path);
        yaml_destroy(doc);
        return AIRY_ERR_INVALID_PARAM;
    }

    route_mode(rt, cfg);
    route_depth(rt, cfg);
    route_on(rt, cfg);
    route_deadline(rt, cfg);
    yaml_destroy(doc);
    return AIRY_OK;
}
