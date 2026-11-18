// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_boot.c
 * @brief 配置读入与落盘的中间态实现（0.1.20 D2-10）。
 *
 * 逐段读入：providers → 价格 → 默认模型 → 路由。任一段缺失只影响该段
 * 语义（价格缺省走内置价、路由缺省保持 off），唯独主配置整体不可读时
 * 置 load_err，让热重载能区分「文件正在被编辑」与「文件确实没配」。
 */

#include "rpc/service_boot.h"

#include "airy_memory.h"
#include "config/pricing_api.h"
#include "config/model_loader.h"
#include "config_attest.h"
#include "daemon_defaults.h"
#include "daemon_platform_ext.h"
#include "rpc/prov_internal.h"
#include "rpc/service.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int llm_boot_usr_path(char *buf, size_t n)
{
    const char *dir = airy_config_dir();
    if (!dir)
        return 0;
    int len = snprintf(buf, n, "%s/model.yaml", dir);
    if (len <= 0 || len >= (int)n)
        return 0;
    FILE *f = fopen(buf, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* 主配置 providers 与用户覆盖合并（同名用户胜出）；失败留空注册表。 */
static void boot_prov_load(const char *cfg, llm_boot_t *b)
{
    int rc = svc_load_model_config(cfg, &b->provs, &b->prov_n);
    if (rc != 0 || !b->provs || b->prov_n == 0) {
        size_t got = b->prov_n;
        free_provider_configs(b->provs, b->prov_n);
        b->provs = NULL;
        b->prov_n = 0;
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL %s (rc=%d, providers=%zu) — registry "
                      "will be empty and every LLM request will fail fast, STACK: "
                      "boot_prov_load",
                      cfg, rc, got);
        return;
    }
    char up[1024];
    provider_config_t *uprov = NULL;
    size_t un = 0;
    if (llm_boot_usr_path(up, sizeof(up)) && svc_load_model_config(up, &uprov, &un) == 0 && uprov &&
        un > 0) {
        provider_config_t *merged = NULL;
        size_t mn = 0;
        merge_provider_configs(b->provs, b->prov_n, uprov, un, &merged, &mn);
        if (merged && mn > 0) {
            free_provider_configs(b->provs, b->prov_n);
            b->provs = merged;
            b->prov_n = mn;
            SVC_LOG_INFO("C-L02: SVC: merged %zu user provider(s) from %s "
                         "(user overrides same-name)",
                         un, up);
        }
    }
    free_provider_configs(uprov, un);
    SVC_LOG_INFO("C-L02: SVC: loaded %zu provider(s) from manager config", b->prov_n);
}

/* llm 段优先、缺省回退 models 表首条（v2 表格格式）；返回来源描述。 */
static const char *boot_pick_cfg(const char *cfg, svc_model_llm_config_t *out)
{
    if (svc_model_defaults_llm_from_yaml(cfg, out) == 0 && out->model[0])
        return "llm section";
    __builtin_memset(out, 0, sizeof(*out));
    if (svc_model_defaults_models0_from_yaml(cfg, out) == 0 && out->model[0])
        return "models table";
    return NULL;
}

/* 默认模型/提供方/输出上限的读取与用户覆盖合并（只读配置，不落 svc）。 */
static void boot_def_load(const char *cfg, llm_boot_t *b)
{
    char up[1024];
    if (cfg) {
        svc_model_defaults_from_yaml(cfg, b->model, sizeof(b->model), b->pname, sizeof(b->pname));
        /* 默认模型的来源与输出上限相互独立：上限仍须从 llm 段（或 models
         * 表首条）读出。 */
        b->src = boot_pick_cfg(cfg, &b->llm);
        /* C-19：槽位与注册表同一次读入；启用位缺省为启用。 */
        b->slots.enabled = 1;
        (void)svc_slot_load(cfg, &b->slots);
    }
    if (!llm_boot_usr_path(up, sizeof(up)))
        return;
    /* 用户覆盖里的槽位逐角色胜出；未写该角色则继承主配置。 */
    svc_model_slots_t us;
    __builtin_memset(&us, 0, sizeof(us));
    us.enabled = 1;
    (void)svc_slot_load(up, &us);
    for (unsigned i = 0; i < SVC_SLOT_N; ++i) {
        const char *v = svc_slot_get(&us, i);
        if (!v || !v[0])
            continue;
        AIRY_STRNCPY_TERM(svc_slot_buf(&b->slots, i), v, SVC_MODEL_ID_MAX);
        if (us.legacy_mask & (1u << i))
            b->slots.legacy_mask |= 1u << i;
    }
    if (us.timeout_ms > 0)
        b->slots.timeout_ms = us.timeout_ms;
    /* 用户只覆盖自己显式写下的 routing 键（不重置主配置）。 */
    (void)svc_route_load(up, &b->route);
    char um[128] = {0};
    char upv[64] = {0};
    svc_model_defaults_from_yaml(up, um, sizeof(um), upv, sizeof(upv));
    if (um[0])
        AIRY_STRNCPY_TERM(b->model, um, sizeof(b->model));
    if (upv[0])
        AIRY_STRNCPY_TERM(b->pname, upv, sizeof(b->pname));

    /* 用户覆盖同样 user wins：调低 max_output 须立即生效。 */
    svc_model_llm_config_t uc;
    __builtin_memset(&uc, 0, sizeof(uc));
    (void)boot_pick_cfg(up, &uc);
    if (uc.model[0] && uc.max_output_tokens > 0)
        b->llm.max_output_tokens = uc.max_output_tokens;
}

/* 价格唯一来源是 model.yaml 的 models[].input/output_cost_per_1k；未配置
 * 价格的模型由 cost_tracker 落默认价。 */
static void boot_price_load(const char *cfg, llm_boot_t *b)
{
    pricing_rule_t *rules = NULL;
    int n = 0;
    if (load_pricing_rules_from_yaml(cfg, &rules, &n) == 0 && rules && n > 0) {
        b->rules = rules;
        b->rule_count = n;
        SVC_LOG_INFO("Loaded %d pricing rules from YAML model config", n);
    } else if (rules) {
        AIRY_FREE(rules);
    }
}

/* 默认模型/提供方/上限落 svc：default_model 段优先，否则取 llm 段来源。 */
static void boot_def_apply(struct llm_service *svc, const llm_boot_t *b)
{
    if (b->model[0]) {
        AIRY_STRNCPY_TERM(svc->default_model, b->model, sizeof(svc->default_model));
        SVC_LOG_INFO("C-L02: SVC: default_model=%s (from default_model config)",
                     svc->default_model);
    } else if (b->src) {
        AIRY_STRNCPY_TERM(svc->default_model, b->llm.model, sizeof(svc->default_model));
        /* 适配器按线格式命名（chat | messages | local），配置面的 api_format
         * 即适配器查找键，无需再做厂商名到适配器的映射。 */
        if (!b->pname[0] && b->llm.api_format[0])
            AIRY_STRNCPY_TERM(svc->default_provider, b->llm.api_format,
                              sizeof(svc->default_provider));
        SVC_LOG_INFO("C-L02: SVC: default_model=%s (from %s)", svc->default_model, b->src);
    }
    if (b->pname[0])
        AIRY_STRNCPY_TERM(svc->default_provider, b->pname, sizeof(svc->default_provider));
    if (b->llm.max_output_tokens > 0) {
        svc->default_max_output_tokens = b->llm.max_output_tokens;
        SVC_LOG_INFO("C-L02: SVC: default_max_output_tokens=%d (from model config)",
                     svc->default_max_output_tokens);
    }
}

void llm_boot_fill(llm_boot_t *b, const char *cfg)
{
    if (!b)
        return;
    __builtin_memset(b, 0, sizeof(*b));
    b->base.llm_cache_capacity = AIRY_DEFAULT_CACHE_CAPACITY;
    b->base.llm_cache_ttl_sec = AIRY_DEFAULT_CACHE_TTL_SEC;
    b->base.max_retries = AIRY_DEFAULT_MAX_RETRIES;
    b->base.timeout_ms = AIRY_DEFAULT_TIMEOUT_MS;
    svc_chain_def(&b->route);

    if (cfg) {
        boot_prov_load(cfg, b);
        b->base.providers = b->provs;
        b->base.provider_count = b->prov_n;
        if (!b->provs || b->prov_n == 0)
            b->load_err = 1;
        boot_price_load(cfg, b);
        (void)svc_route_load(cfg, &b->route);
    }
    /* 用户覆盖独立于主配置路径：cfg 缺席时 $AIRY_CONFIG_DIR 仍生效。 */
    boot_def_load(cfg, b);
}

void llm_boot_drop(llm_boot_t *b)
{
    if (!b)
        return;
    free_provider_configs(b->provs, b->prov_n);
    b->provs = NULL;
    b->prov_n = 0;
    if (b->rules) {
        free_pricing_rules(b->rules, b->rule_count);
        b->rules = NULL;
        b->rule_count = 0;
    }
}

void llm_boot_apply(struct llm_service *svc, llm_boot_t *b)
{
    if (!svc || !b)
        return;
    boot_def_apply(svc, b);
    svc->slots = b->slots;
    svc->slots_degraded = 0;
    for (unsigned i = 0; i < SVC_SLOT_N; ++i) {
        const char *v = svc_slot_get(&b->slots, i);
        if (!v || !v[0])
            svc->slots_degraded = 1;
    }
    svc->route = b->route;
    svc->rules = b->rules;
    svc->rule_count = b->rule_count > 0 ? (size_t)b->rule_count : 0;
    b->rules = NULL;
    b->rule_count = 0;
}

/* 逃生开关：仅排障用。只把「不闭合」降级为告警，不放宽对账本身。 */
int llm_boot_unknown(void)
{
    const char *esc = getenv("AIRY_ALLOW_UNKNOWN_MODEL");
    return (esc && esc[0] && strcmp(esc, "0") != 0) ? 1 : 0;
}

static int boot_reg_hit(const struct llm_service *svc, const char *id)
{
    if (!id || !id[0])
        return AIRY_ATTEST_REG_NA;
    return provider_registry_find(svc->registry, id) ? AIRY_ATTEST_REG_HIT : AIRY_ATTEST_REG_MISS;
}

int llm_boot_attest(struct llm_service *svc)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;

    /* 未声明 default_model 是「没配」，不是「配错」：不做注册对账，
     * 由请求侧按需回落到 provider 默认模型。 */
    airy_attest_item_t items[1 + SVC_SLOT_N];
    size_t n = 0;
    items[n++] = (airy_attest_item_t){
        .key = "default_model",
        .declared = svc->default_model,
        .effective = svc->default_model,
        .in_registry = boot_reg_hit(svc, svc->default_model),
    };
    /* C-19：槽位引用同样是「声明」，必须与注册表对账；空槽位不参与对账，
     * 其语义是回落 default_model（由 slots_degraded 显式上报）。 */
    for (unsigned i = 0; i < SVC_SLOT_N; ++i) {
        const char *v = svc_slot_get(&svc->slots, i);
        if (!v || !v[0])
            continue;
        items[n++] = (airy_attest_item_t){
            .key = svc_slot_name(i),
            .declared = v,
            .effective = v,
            .in_registry = boot_reg_hit(svc, v),
        };
    }
    airy_attest_opts_t opts = {.owner = "llm_d", .allow_unknown = llm_boot_unknown()};
    airy_attest_result_t res;
    int rc = airy_attest_run(&opts, items, n, &res);

    if (svc->slots_degraded)
        SVC_LOG_WARN("C-19: llm_d slots degraded — 未显式配置的角色回落 default_model=%s "
                     "(t2=%s t1f=%s t1p=%s)",
                     svc->default_model, svc->slots.t2[0] ? svc->slots.t2 : "-",
                     svc->slots.t1f[0] ? svc->slots.t1f : "-",
                     svc->slots.t1p[0] ? svc->slots.t1p : "-");
    if (rc == AIRY_SUCCESS) {
        SVC_LOG_INFO("C-19: llm_d slots effective t2=%s t1f=%s t1p=%s",
                     svc->slots.t2[0] ? svc->slots.t2 : svc->default_model,
                     svc->slots.t1f[0] ? svc->slots.t1f : svc->default_model,
                     svc->slots.t1p[0] ? svc->slots.t1p : svc->default_model);
        return 0;
    }

    SVC_LOG_ERROR("llm_d: 配置生效自证未闭合，拒绝启动（default_model='%s' 或槽位引用不在注册表）",
                  svc->default_model);
    return -1;
}
