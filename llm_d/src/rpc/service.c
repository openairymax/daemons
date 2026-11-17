// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file service.c
 * @brief LLM service core logic implementation.
 *
 * Improvements:
 * 1. Fixed the non-portable stpcpy issue
 * 2. Unified error codes to AIRY_ERR_*
 * 3. Completed the YAML parsing logic
 * 4. Thread safety
 */

#include "daemon_defaults.h"
#include "error.h"
#include "airy_time.h"
#include "daemon_platform_ext.h"
#include "response.h"
#include "router/core/llm_router.h"
#include "service.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/pricing_api.h"
#include "airy_prov/provider_base.h"
#include "airy_prov/provider_rec.h"
#include "airy_prov/prov_degrade.h"
#include "airy_prov/prov_stat.h"
#include "airy_prov/registry.h"
#include "rpc/prov_internal.h"
#include "rpc/service_boot.h"
#include "rpc/service_chain.h"
#include "sync.h"

/* 装配头：仅用于可见 llm_svc_reg 原型（防签名漂移）。 */
#include "llm_d_internal.h"

/* 跨域桥（0.1.19 D5）：向装配域/RPC 方法域暴露 provider registry 句柄，
 * 使 struct llm_service 保持私有不被穿透。见 llm_d_internal.h 契约注释。 */
provider_registry_t *llm_svc_reg(const llm_service_t *svc)
{
    if (!svc)
        return NULL;
    const struct llm_service *s = (const struct llm_service *)svc;
    return s->registry;
}

/* 跨域桥（0.1.20 FanOut）：routing 配置按值出域，service_chain 由此不再
 * 解引用 struct llm_service，保持纯机制件身份。 */
void svc_chain_ctx(const llm_service_t *svc, llm_route_cfg_t *out)
{
    if (!out)
        return;
    if (!svc) {
        svc_chain_def(out);
        return;
    }
    *out = ((const struct llm_service *)svc)->route;
}

/* 跨域桥（0.1.20 FanOut）：注册表精确查找留在 rpc 域内，锁粒度与 0.1.19
 * 一致（持 svc->lock 查表，返回的 provider 由注册表世代保证存活）。 */
const provider_t *svc_find_prov(llm_service_t *svc, const char *model)
{
    if (!svc || !model)
        return NULL;
    struct llm_service *s = (struct llm_service *)svc;
    airy_mtx_lock(&s->lock);
    const provider_t *prov = provider_registry_find(s->registry, model);
    airy_mtx_unlock(&s->lock);
    return prov;
}

/* 跨域桥（0.1.21 B-1）：按能力位选路。只把「要不要流式」作为硬要求，
 * 加速器/显存等由 B-3 探测后经 provider_select_t 扩展；能力位未声明的
 * 候选仍可命中（保守可用），与旧「按名字取第一个」在缺省配置下等价。 */
const provider_t *svc_select_prov(llm_service_t *svc, const char *model, bool need_stream)
{
    if (!svc || !model)
        return NULL;
    struct llm_service *s = (struct llm_service *)svc;
    provider_select_t sel;

    __builtin_memset(&sel, 0, sizeof(sel));
    sel.model = model;
    sel.need_stream = need_stream ? 1 : 0;
    airy_mtx_lock(&s->lock);
    const provider_t *prov = provider_registry_select(s->registry, &sel);
    airy_mtx_unlock(&s->lock);
    return prov;
}

/* 跨域桥（0.1.20 P-A）：请求层据此把「没有 provider」与「模型未配置」分开。 */
bool svc_reg_empty(llm_service_t *svc)
{
    if (!svc)
        return true;
    struct llm_service *s = (struct llm_service *)svc;
    airy_mtx_lock(&s->lock);
    size_t n = provider_reg_count(s->registry);
    airy_mtx_unlock(&s->lock);
    return n == 0;
}

/* 跨域桥（0.1.20 D2-7）：装配域按固定周期驱动检测、按信号触发应用，
 * struct llm_service 本身不出 rpc 域。 */
int llm_svc_cfg_tick(llm_service_t *svc, uint64_t now_ms)
{
    struct llm_service *s = (struct llm_service *)svc;
    if (!s)
        return -1;

    int hit = 0;
    if (svc_mw_tick(&s->mw, now_ms) > 0)
        hit = 1;
    if (svc_mw_tick(&s->mw_usr, now_ms) > 0)
        hit = 1;
    return hit;
}

int llm_svc_cfg_hup(llm_service_t *svc)
{
    struct llm_service *s = (struct llm_service *)svc;
    return s ? svc_mw_reload(&s->mw) : -1;
}

/* 降级阶梯位阶表（B-4）：enumerate 持注册表锁，回调内不得再查注册表，
 * 故先按 provider 快照算好位阶（GPU→CPU→远端），再在登记闭包里按名字
 * 取用。provider 数量上限与熔断账目表一致，超出者按 NONE 处理。 */
#define SVC_DEG_MAX 64

typedef struct {
    char name[64];
    int rank;
} deg_slot_t;

typedef struct {
    deg_slot_t slots[SVC_DEG_MAX];
    size_t count;
} deg_tbl_t;

static int deg_snap_cb(const provider_t *prov, void *ud)
{
    deg_tbl_t *t = (deg_tbl_t *)ud;
    if (!prov || !prov->name)
        return 0;
    if (t->count >= SVC_DEG_MAX) {
        SVC_LOG_WARN("C-L02: SVC: degrade table full at %d, remaining providers rank NONE",
                     SVC_DEG_MAX);
        return 1;
    }
    deg_slot_t *slot = &t->slots[t->count++];
    AIRY_STRNCPY_TERM(slot->name, prov->name, sizeof(slot->name));
    slot->rank = (int)prov_deg_rank(prov);
    return 0;
}

static int deg_rank_of(const deg_tbl_t *t, const char *name)
{
    if (!t || !name)
        return (int)PROV_DEG_NONE;
    for (size_t i = 0; i < t->count; ++i) {
        if (strcmp(t->slots[i].name, name) == 0)
            return t->slots[i].rank;
    }
    return (int)PROV_DEG_NONE;
}

/* 端点登记闭包：注册表遍历期持锁，回调只拷名字与能力位，不持有 provider
 * 指针。C-1：context_window 取 provider 能力位（由模型 context_window /
 * max_context 声明派生）而非 8192 硬编码，使声明容量真正进入选路。 */
typedef struct {
    const pricing_rule_t *rules;
    int rule_count;
    int registered;
    const deg_tbl_t *deg;
} router_reg_t;

static void register_one_endpoint(router_reg_t *reg, const char *provider_name,
                                  const char *model_name, uint32_t context_window)
{
    llm_endpoint_t ep;
    __builtin_memset(&ep, 0, sizeof(ep));
    snprintf(ep.provider_name, sizeof(ep.provider_name), "%s", provider_name);
    snprintf(ep.model_name, sizeof(ep.model_name), "%s", model_name);
    ep.enabled = true;
    /* 位阶即优先级：GPU(0) > CPU(-1) > 远端(-2) > 未知(-3)。策略层未显式
     * 指定顺序时的默认阶梯，配置显式优先级可在 router 侧覆盖。 */
    ep.priority = -deg_rank_of(reg->deg, provider_name);
    ep.context_window = (context_window > 0) ? (int)context_window : 8192;
    ep.avg_latency_ms = 1000;
    ep.capabilities = LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING;
    cost_tracker_price(reg->rules, reg->rule_count, model_name, &ep.cost_per_1k_input,
                       &ep.cost_per_1k_output);
    int rc = llm_router_register_endpoint(&ep);
    if (rc == 0) {
        reg->registered++;
    } else {
        SVC_LOG_WARN("C-L02: SVC: router endpoint register FAILED for %s/%s (rc=%d)", provider_name,
                     model_name, rc);
    }
}

static int register_endpoint_cb(const provider_t *prov, void *user_data)
{
    router_reg_t *reg = (router_reg_t *)user_data;
    if (!prov || !prov->name || !prov->models)
        return 0;
    uint32_t cap = (prov->hw.declared && prov->hw.max_context > 0) ? prov->hw.max_context : 0u;
    for (char **m = prov->models; *m; ++m) {
        if (!**m)
            continue;
        register_one_endpoint(reg, prov->name, *m, cap);
    }
    return 0;
}

void register_router_endpoints(llm_service_t *svc)
{
    struct llm_service *s = (struct llm_service *)svc;
    if (!s)
        return;
    deg_tbl_t deg;
    __builtin_memset(&deg, 0, sizeof(deg));
    provider_reg_walk(s->registry, deg_snap_cb, &deg);
    router_reg_t reg = {(const pricing_rule_t *)s->rules, (int)s->rule_count, 0, &deg};
    provider_reg_walk(s->registry, register_endpoint_cb, &reg);
    SVC_LOG_INFO("C-L02: SVC: registered %d endpoints into llm_router", reg.registered);
}

/* 计费/用量持久化路径 $AIRY_DATA_DIR/agentrt/llm_usage.json，重启不清零。 */
const char *llm_usage_state_path(void)
{
    static char path[1024];
    static int built;
    if (built)
        return path;
    const char *data_dir = airy_data_dir();
    char dir[1024];
    int dlen = data_dir ? snprintf(dir, sizeof(dir), "%s/agentrt", data_dir) : -1;
    if (dlen <= 0 || dlen >= (int)sizeof(dir) || airy_mkdir_p(dir) != 0)
        return NULL;
    int plen = snprintf(path, sizeof(path), "%s/llm_usage.json", dir);
    if (plen <= 0 || plen >= (int)sizeof(path))
        return NULL;
    built = 1;
    return path;
}

/* 组件装配：注册表 → 缓存 → 计费；任一失败由调用方统一回滚。 */
static int llm_parts_new(llm_service_t *svc, const service_config_t *base)
{
    svc->registry = provider_registry_create(base);
    if (!svc->registry) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL registry, STACK: llm_service_create");
        return -1;
    }
    svc->cache = cache_create_string_cache(base->llm_cache_capacity, base->llm_cache_ttl_sec);
    if (!svc->cache) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL cache, STACK: llm_service_create");
        return -1;
    }
    svc->cost = cost_tracker_create((const pricing_rule_t *)svc->rules, (int)svc->rule_count);
    if (!svc->cost) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL cost_tracker, STACK: llm_service_create");
        return -1;
    }
    return 0;
}

/* 创建失败回滚：各 destroy 对 NULL 安全，按装配反序释放。 */
static void llm_parts_drop(llm_service_t *svc)
{
    cost_tracker_destroy(svc->cost);
    cache_destroy(svc->cache);
    provider_registry_destroy(svc->registry);
    free_pricing_rules((pricing_rule_t *)svc->rules, (int)svc->rule_count);
    svc_swap_done(&svc->swap);
    airy_mtx_destroy(&svc->reload_lock);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

/* 路由是进程级单例（llm_router.c）且 init 幂等；失败非致命，complete
 * 路径回退 find_provider。 */
static void llm_route_init(llm_service_t *svc)
{
    if (llm_router_init() != 0) {
        SVC_LOG_WARN(
            "C-L02: SVC: llm_router_init failed — routing disabled, falling back to find_provider");
        return;
    }
    register_router_endpoints(svc);
}

/* SIGHUP 应用回调（0.1.20 D2-7）：机制件只负责触发，切换语义全在本域。
 * 成功时副监视（用户覆盖）就地重照，新建的覆盖文件亦在此被纳入监视；
 * 主监视由 svc_mw_reload 自行 rebase，避免同一文件读两遍。 */
static int llm_cfg_apply(void *ud, char *why, size_t cap)
{
    llm_service_t *svc = (llm_service_t *)ud;
    if (!svc)
        return -1;

    int rc = llm_service_reload(svc);
    if (rc == AIRY_OK) {
        char up[1024];
        if (llm_boot_usr_path(up, sizeof(up)) &&
            strcmp(up, svc->cfg_path) != 0)
            (void)svc_mw_rebase(&svc->mw_usr, up);
        return 0;
    }
    if (rc == 1)
        return 1; /* 忙/退避：不产生失败事件，等下一次 SIGHUP */
    if (why && cap > 0)
        snprintf(why, cap, "reload rejected rc=%d", rc);
    return -1;
}

/* 0.1.19 §3.5 缺陷5 / 0.1.20 D2-7：对启动时真正消费的两份 model.yaml
 * 照快照并挂应用回调。manager 配置与用户覆盖 (A2-1) 只要路径不同即各
 * 照一份；缺席者 valid=0 静默停用，但 SIGHUP 仍可补救（回调已挂）。 */
static void llm_watch_cfg(llm_service_t *svc, const char *cfg)
{
    char up[1024];
    if (cfg) {
        (void)svc_mw_init(&svc->mw, cfg, "llm_d");
        /* 周期 0 = 机制件缺省（SVC_MW_INTERVAL_MS），与装配域的 tick 同源 */
        (void)svc_mw_hook(&svc->mw, llm_cfg_apply, svc, 0);
    }
    if (llm_boot_usr_path(up, sizeof(up)) && (!cfg || strcmp(cfg, up) != 0))
        (void)svc_mw_init(&svc->mw_usr, up, "llm_d");
}

llm_service_t *llm_service_create(const char *config_path)
{
    llm_service_t *svc = (llm_service_t *)AIRY_CALLOC(1, sizeof(llm_service_t));
    if (!svc) {
        SVC_LOG_ERROR(
            "C-L02: SVC: CREATE-FAIL allocate service context, STACK: llm_service_create");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* CALLOC 全零会让 depth=0，必须显式落缺省（0.1.19 §3.8.4 A3）。 */
    svc_chain_def(&svc->route);

    if (airy_mtx_init(&svc->lock) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL init lock, STACK: llm_service_create");
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (airy_mtx_init(&svc->reload_lock) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL init reload_lock, STACK: llm_service_create");
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (svc_swap_init(&svc->swap, SVC_SWAP_DRAIN_MS) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL init swap, STACK: llm_service_create");
        airy_mtx_destroy(&svc->reload_lock);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (config_path)
        AIRY_STRNCPY_TERM(svc->cfg_path, config_path, sizeof(svc->cfg_path));

    /* 0.1.19 §3.8.4 A4：routing 段随 boot 一次读入；缺失即保持 off。 */
    llm_boot_t b;
    llm_boot_fill(&b, config_path);
    llm_boot_apply(svc, &b);

    if (svc->route.mode == SVC_CHAIN_AUTO)
        SVC_LOG_INFO("C-L02: SVC: routing=auto depth=%d on=0x%x budget_ms=%d", svc->route.depth,
                     svc->route.on_mask, svc->route.budget_ms);

    if (llm_parts_new(svc, &b.base) != 0) {
        llm_boot_drop(&b);
        llm_parts_drop(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    /* 注册表已深拷贝 provider 配置，暂存态（含用户覆盖合并结果）释放；
     * 空注册表的 ERROR 证据由 boot_prov_load 在根因处一次性留证。 */
    llm_boot_drop(&b);

    /* 加载历史计费/用量累计（幂等，无文件则跳过）。 */
    const char *usage_path = llm_usage_state_path();
    if (usage_path) {
        if (cost_tracker_load(svc->cost, usage_path) == 0)
            SVC_LOG_INFO("C-L02: SVC: cost state loaded from %s", usage_path);
        else
            SVC_LOG_WARN("C-L02: SVC: cost state load failed (ignored) path=%s", usage_path);
    }

    llm_route_init(svc);
    llm_watch_cfg(svc, config_path);

    /* 0.1.21 A-8：声明 = 注册 = 生效。未闭合即拒绝启动，不以默认值兜底；
     * 逃生开关只降级为 ESCAPED 告警，不放宽对账本身。 */
    if (llm_boot_attest(svc) != 0) {
        llm_service_destroy(svc);
        AIRY_ERROR_NULL(AIRY_ERR_SVC_CONFIG, "config attestation rejected");
    }

    SVC_LOG_INFO("C-L02: SVC: CREATE-OK pricing_rules=%d llm_cache_capacity=%zu cache_ttl=%u",
                 (int)svc->rule_count, b.base.llm_cache_capacity, b.base.llm_cache_ttl_sec);
    return svc;
}

void llm_service_destroy(llm_service_t *svc)
{
    if (!svc)
        return;

    SVC_LOG_INFO("C-L02: SVC: DESTROY");

    /* 退休表：热重载排空超时后旧装载仍未归零，只能留到这里统一释放。 */
    for (int i = 0; i < svc->retired_n; i++) {
        if (svc->retired[i].registry)
            provider_registry_destroy(svc->retired[i].registry);
        if (svc->retired[i].rules)
            free_pricing_rules(svc->retired[i].rules, svc->retired[i].rule_count);
    }
    svc->retired_n = 0;

    if (svc->registry) {
        provider_registry_destroy(svc->registry);
        svc->registry = NULL;
    }

    if (svc->cache) {
        cache_destroy(svc->cache);
        svc->cache = NULL;
    }

    if (svc->cost) {
        /* 退出前保存计费/用量累计，重启后不清零。 */
        const char *usage_path = llm_usage_state_path();
        if (usage_path && cost_tracker_save(svc->cost, usage_path) == 0)
            SVC_LOG_INFO("C-L02: SVC: cost state saved to %s", usage_path);
        cost_tracker_destroy(svc->cost);
        svc->cost = NULL;
    }

    if (svc->rules) {
        free_pricing_rules((pricing_rule_t *)svc->rules, (int)svc->rule_count);
        svc->rules = NULL;
        svc->rule_count = 0;
    }

    /* Destroy the global router singleton.
     * Note: the router is a process-level global singleton; this assumes
     * llm_service is a single instance in the daemon (paired with
     * llm_router_init on the create path). llm_router_init is idempotent, so
     * it can be re-initialized when the service is recreated. Each test is a
     * separate executable process, so there is no cross-instance global-state
     * pollution. */
    llm_router_destroy();

    svc_swap_done(&svc->swap);
    airy_mtx_destroy(&svc->reload_lock);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

/* D-1：自托管观测面单次快照所需端点事实。全部取自 live 计数与探测结果，
 * 不做任何常量占位——面板与闸/日志同源，避免「显示空闲、实际排队」。 */
typedef struct {
    cJSON *arr;
} svc_ep_ctx_t;

static int svc_ep_cb(const provider_t *prov, void *ud)
{
    svc_ep_ctx_t *ctx = (svc_ep_ctx_t *)ud;
    if (!ctx || !ctx->arr || !prov || !prov->name)
        return 0;

    provider_base_ctx_t *base = prov->ctx ? provider_base_ctx(prov->ctx) : NULL;
    cJSON *ep = cJSON_CreateObject();
    if (!ep)
        return 0;

    cJSON_AddStringToObject(ep, "provider", prov->name);
    const char *first = (prov->models && prov->models[0]) ? prov->models[0] : "";
    cJSON *models = cJSON_CreateArray();
    if (models) {
        for (size_t i = 0; prov->models && prov->models[i]; i++)
            cJSON_AddItemToArray(models, cJSON_CreateString(prov->models[i]));
        cJSON_AddItemToObject(ep, "models", models);
    }
    cJSON_AddNumberToObject(ep, "capacity", (double)svc_chain_cap(prov, first));
    if (base) {
        cJSON_AddStringToObject(ep, "api_base", base->api_base);
        cJSON_AddNumberToObject(ep, "props_state", base->props_state);
        cJSON_AddNumberToObject(ep, "total_slots", base->total_slots);
        cJSON_AddNumberToObject(ep, "n_ctx_slot", base->n_ctx_slot);
        cJSON_AddNumberToObject(ep, "inflight", (double)sync_atomic_load(&base->inflight));
        cJSON_AddNumberToObject(ep, "waiting", (double)sync_atomic_load(&base->waiting));
        cJSON_AddBoolToObject(ep, "backend_ready", base->backend_ready != 0);
        cJSON_AddStringToObject(ep, "backend_reason", base->backend_reason);
    }
    cJSON_AddItemToArray(ctx->arr, ep);
    return 0;
}

int llm_service_stats(llm_service_t *svc, char **out_json)
{
    if (!svc || !out_json) {
        SVC_LOG_ERROR("llm_service_stats: NULL parameter (svc=%p, out_json=%p)", (const void *)svc,
                      (const void *)out_json);
        return AIRY_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        SVC_LOG_ERROR("llm_service_stats: cJSON_CreateObject failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *cost_json = cost_tracker_export(svc->cost);
    if (cost_json) {
        cJSON_AddItemToObject(root, "cost", cost_json);
    }

    cJSON_AddNumberToObject(root, "llm_cache_size", cache_get_size(svc->cache));
    cJSON_AddNumberToObject(root, "llm_cache_capacity", cache_get_capacity(svc->cache));

    /* D-7（Q19）：一次快照出四元组，调用方无需再从多个计数器拼失败率。 */
    uint64_t rq_total, rq_ok, rq_fail, rq_timeout;
    airy_mtx_lock(&svc->lock);
    rq_total = svc->req_total;
    rq_ok = svc->req_ok;
    rq_fail = svc->req_fail;
    rq_timeout = svc->req_timeout;
    airy_mtx_unlock(&svc->lock);
    cJSON *rq = cJSON_CreateObject();
    if (rq) {
        cJSON_AddNumberToObject(rq, "total", (double)rq_total);
        cJSON_AddNumberToObject(rq, "ok", (double)rq_ok);
        cJSON_AddNumberToObject(rq, "fail", (double)rq_fail);
        cJSON_AddNumberToObject(rq, "timeout", (double)rq_timeout);
        cJSON_AddNumberToObject(rq, "success_rate",
                                rq_total ? (double)rq_ok / (double)rq_total : 0.0);
        cJSON_AddItemToObject(root, "requests", rq);
    }

    cache_stats_t cache_stats;
    cache_get_stats(svc->cache, &cache_stats);
    cJSON_AddNumberToObject(root, "llm_cache_hits", (double)cache_stats.hits);
    cJSON_AddNumberToObject(root, "llm_cache_misses", (double)cache_stats.misses);
    cJSON_AddNumberToObject(root, "llm_cache_evictions", (double)cache_stats.evictions);
    cJSON_AddNumberToObject(root, "llm_cache_hit_rate", cache_stats.hit_rate);

    llm_router_stats_t rs;
    if (llm_router_get_stats(&rs) == 0) {
        static const char *const k_route[] = {"complexity", "cost",   "latency",
                                              "fallback",   "round_robin"};
        cJSON *router = cJSON_CreateObject();
        if (router) {
            cJSON_AddNumberToObject(router, "total_requests", (double)rs.total_requests);
            for (size_t i = 0; i < sizeof(k_route) / sizeof(k_route[0]); i++) {
                char key[32];
                snprintf(key, sizeof(key), "routed_%s", k_route[i]);
                cJSON_AddNumberToObject(router, key, (double)rs.routed_count[i]);
            }
            cJSON_AddNumberToObject(router, "fallback_count", (double)rs.fallback_count);
            cJSON_AddNumberToObject(router, "error_count", (double)rs.error_count);
            cJSON_AddNumberToObject(router, "total_cost", rs.total_cost);
            cJSON_AddNumberToObject(router, "total_tokens", (double)rs.total_tokens);
            cJSON_AddItemToObject(root, "router", router);
        }
        cJSON *meas = cJSON_CreateObject();
        if (meas) {
            cJSON_AddNumberToObject(meas, "lat_samples", (double)rs.lat_samples);
            cJSON_AddNumberToObject(meas, "lat_total_ms", (double)rs.lat_total_ms);
            cJSON_AddNumberToObject(meas, "avg_latency_ms",
                                    rs.lat_samples ? (double)rs.lat_total_ms /
                                                         (double)rs.lat_samples
                                                   : 0.0);
            cJSON_AddNumberToObject(meas, "prompt_tokens", (double)rs.prompt_tokens_sum);
            cJSON_AddNumberToObject(meas, "cached_tokens", (double)rs.cached_tokens_sum);
            /* G47：分列与样本数并出，禁止以 aggregate 冒充性能；判定层为端点自报。 */
            cJSON_AddNumberToObject(meas, "meas_samples", (double)rs.meas_samples);
            cJSON_AddNumberToObject(meas, "prefill_tokens", (double)rs.prefill_tokens);
            cJSON_AddNumberToObject(meas, "decode_tokens", (double)rs.decode_tokens);
            cJSON_AddNumberToObject(meas, "kv_cache_tokens", (double)rs.kv_cache_tokens);
            cJSON_AddNumberToObject(meas, "prefill_ms", rs.prefill_ms);
            cJSON_AddNumberToObject(meas, "decode_ms", rs.decode_ms);
            cJSON_AddItemToObject(root, "measurement", meas);
        }
    }

    /* D2-9：超时三拆的传输侧观测。三类成因处置不同——连接未建立可无条件
     * 重试、上游停摆仅在幂等时可重试、整轮预算耗尽不可重试；混为一个
     * "超时"计数会掩盖根因。 */
    cJSON *transport = cJSON_CreateObject();
    if (transport) {
        cJSON_AddNumberToObject(transport, "connect_timeout",
                                (double)prov_stat_get(PROV_TMO_CONNECT));
        cJSON_AddNumberToObject(transport, "stream_stall",
                                (double)prov_stat_get(PROV_TMO_STALL));
        cJSON_AddNumberToObject(transport, "total_timeout",
                                (double)prov_stat_get(PROV_TMO_TOTAL));
        cJSON_AddItemToObject(root, "transport", transport);
    }

    /* D2-10：热切换观测。gen 与 mismatch 并出——只见 gen 增长而看不到被
     * 丢弃的旧世代回包，会把强制排空误读为"切换无损"。 */
    cJSON *swap = cJSON_CreateObject();
    if (swap) {
        uint32_t gen = 0;
        uint32_t inflight = 0;
        uint32_t swapped = 0;
        uint64_t mismatch = 0;
        uint64_t drain_tmo = 0;
        svc_swap_stat(&svc->swap, &gen, &inflight, &swapped, &mismatch, &drain_tmo);
        cJSON_AddNumberToObject(swap, "gen", (double)gen);
        cJSON_AddNumberToObject(swap, "inflight", (double)inflight);
        cJSON_AddNumberToObject(swap, "swapped", (double)swapped);
        cJSON_AddNumberToObject(swap, "gen_mismatch", (double)mismatch);
        cJSON_AddNumberToObject(swap, "drain_timeout", (double)drain_tmo);
        cJSON_AddNumberToObject(swap, "reload_ok", (double)svc->reload_ok);
        cJSON_AddNumberToObject(swap, "reload_fail", (double)svc->reload_fail);
        cJSON_AddItemToObject(root, "swap", swap);
    }

    /* D-1：自托管端点快照（模型/后端/并发容量/在飞/排队/后端就绪）。 */
    if (svc->registry) {
        cJSON *eps = cJSON_CreateArray();
        if (eps) {
            svc_ep_ctx_t epc = {eps};
            provider_reg_walk(svc->registry, svc_ep_cb, &epc);
            cJSON_AddItemToObject(root, "endpoints", eps);
        }
    }
    /* D-1：最近一次失败（模型 + 错误码 + 同一码的权威描述 + 距今毫秒）。
     * 用 llm_d 自身单调钟求差：TUI 的单调钟起点不同，传绝对时刻无法
     * 跨进程对读。 */
    if (svc->last_err_ms) {
        cJSON *le = cJSON_CreateObject();
        if (le) {
            uint64_t now = airy_time_ms();
            cJSON_AddStringToObject(le, "model", svc->last_err_model);
            cJSON_AddNumberToObject(le, "rc", (double)svc->last_err_rc);
            cJSON_AddStringToObject(le, "reason", airy_err_str(svc->last_err_rc));
            cJSON_AddNumberToObject(le, "age_ms",
                                    (double)(now > svc->last_err_ms ? now - svc->last_err_ms : 0));
            cJSON_AddItemToObject(root, "last_error", le);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        SVC_LOG_ERROR("llm_service_stats: cJSON_PrintUnformatted failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    *out_json = json;
    return AIRY_OK;
}

typedef struct {
    cJSON *models_arr;
    const char *default_model;
} list_models_ctx_t;

static int list_models_cb(const char *provider_name, const char *model_name, void *user_data)
{
    list_models_ctx_t *ctx = (list_models_ctx_t *)user_data;
    if (!ctx || !ctx->models_arr || !provider_name || !model_name)
        return 0;

    cJSON *item = cJSON_CreateObject();
    if (!item)
        return 0;
    cJSON_AddStringToObject(item, "name", model_name);
    cJSON_AddStringToObject(item, "provider", provider_name);
    cJSON_AddBoolToObject(item, "default",
                          ctx->default_model && strcmp(ctx->default_model, model_name) == 0);
    cJSON_AddItemToArray(ctx->models_arr, item);
    return 0;
}

/* C-19：把「模型 → 角色」标到每个模型条目上，让 CLI/TUI 不必自己拼
 * model_id 字符串；effective 取值同时给出，空槽位回落 default_model。 */
static void list_mark_slots(cJSON *models_arr, const struct llm_service *svc)
{
    const char *eff[SVC_SLOT_N];
    for (unsigned r = 0; r < SVC_SLOT_N; ++r) {
        const char *v = svc_slot_get(&svc->slots, r);
        eff[r] = (v && v[0]) ? v : svc->default_model;
    }
    int n = cJSON_GetArraySize(models_arr);
    for (int i = 0; i < n; ++i) {
        cJSON *it = cJSON_GetArrayItem(models_arr, i);
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        if (!nm)
            continue;
        cJSON *roles = cJSON_CreateArray();
        if (!roles)
            continue;
        for (unsigned r = 0; r < SVC_SLOT_N; ++r) {
            if (eff[r] && eff[r][0] && strcmp(nm, eff[r]) == 0)
                cJSON_AddItemToArray(roles, cJSON_CreateString(svc_slot_name(r)));
        }
        if (cJSON_GetArraySize(roles) > 0)
            cJSON_AddItemToObject(it, "slots", roles);
        else
            cJSON_Delete(roles);
    }
}

char *llm_service_list_models(llm_service_t *svc)
{
    if (!svc)
        return NULL;

    airy_mtx_lock(&svc->lock);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    cJSON *models_arr = cJSON_CreateArray();
    if (!models_arr) {
        cJSON_Delete(root);
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    cJSON_AddItemToObject(root, "models", models_arr);

    list_models_ctx_t ctx = {
        .models_arr = models_arr,
        .default_model = svc->default_model[0] ? svc->default_model : NULL,
    };
    provider_registry_enumerate(svc->registry, list_models_cb, &ctx);

    cJSON_AddStringToObject(root, "default_model", svc->default_model[0] ? svc->default_model : "");
    if (svc->default_provider[0])
        cJSON_AddStringToObject(root, "default_provider", svc->default_provider);

    cJSON *slots = cJSON_CreateObject();
    if (slots) {
        cJSON_AddBoolToObject(slots, "degraded", svc->slots_degraded != 0);
        for (unsigned r = 0; r < SVC_SLOT_N; ++r) {
            const char *v = svc_slot_get(&svc->slots, r);
            cJSON *one = cJSON_CreateObject();
            if (!one)
                continue;
            cJSON_AddStringToObject(one, "declared", v ? v : "");
            cJSON_AddStringToObject(one, "effective",
                                    (v && v[0]) ? v : svc->default_model);
            cJSON_AddItemToObject(slots, svc_slot_name(r), one);
        }
        cJSON_AddItemToObject(root, "slots", slots);
    }
    list_mark_slots(models_arr, svc);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    airy_mtx_unlock(&svc->lock);
    return json;
}

/* 槽位落盘位置：think_d 与用户覆盖都只认 $AIRY_CONFIG_DIR/model.yaml，
 * 故优先写该文件（不存在则创建 slots-only 文件）；没有配置目录时退回
 * 服务启动时的主配置路径。 */
static int slots_target(const llm_service_t *svc, char *buf, size_t cap)
{
    const char *dir = airy_config_dir();
    if (dir) {
        int n = snprintf(buf, cap, "%s/model.yaml", dir);
        if (n > 0 && (size_t)n < cap)
            return 0;
    }
    if (svc->cfg_path[0]) {
        AIRY_STRNCPY_TERM(buf, svc->cfg_path, cap);
        return 0;
    }
    return -1;
}

static int slots_role(const char *role)
{
    if (!role || !role[0])
        return -1;
    for (unsigned r = 0; r < SVC_SLOT_N; ++r) {
        if (strcmp(role, svc_slot_name(r)) == 0)
            return (int)r;
    }
    return -1;
}

/* model_id 语法：非空、长度受限、无引号/控制字符（写入 YAML 前把关，
 * 避免用转义引入第二种编码）。 */
static int slots_id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= SVC_MODEL_ID_MAX)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (c < 0x20 || c == 0x7f || c == '"')
            return 0;
    }
    return 1;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t off;
} slot_names_t;

static int slots_name_cb(const char *prov, const char *model, void *ud)
{
    slot_names_t *s = (slot_names_t *)ud;
    (void)prov;
    if (!s || !model)
        return 0;
    int n = snprintf(s->buf + s->off, s->cap - s->off, "%s%s", s->off ? "," : "", model);
    if (n > 0 && (size_t)n < s->cap - s->off)
        s->off += (size_t)n;
    return 0;
}

char *llm_service_slots_get(llm_service_t *svc)
{
    if (!svc)
        return NULL;
    airy_mtx_lock(&svc->lock);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    cJSON_AddBoolToObject(root, "degraded", svc->slots_degraded != 0);
    cJSON_AddStringToObject(root, "default_model",
                            svc->default_model[0] ? svc->default_model : "");
    for (unsigned r = 0; r < SVC_SLOT_N; ++r) {
        const char *v = svc_slot_get(&svc->slots, r);
        cJSON *one = cJSON_CreateObject();
        if (!one)
            continue;
        cJSON_AddStringToObject(one, "declared", v ? v : "");
        cJSON_AddStringToObject(one, "effective", (v && v[0]) ? v : svc->default_model);
        cJSON_AddItemToObject(root, svc_slot_name(r), one);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    airy_mtx_unlock(&svc->lock);
    return json;
}

int llm_service_slots_set(llm_service_t *svc, const char *role, const char *model, char *err,
                          size_t cap)
{
    if (!svc || !err || cap == 0)
        return AIRY_ERR_INVALID_PARAM;
    err[0] = '\0';
    int r = slots_role(role);
    if (r < 0) {
        snprintf(err, cap, "unknown slot '%s' (expect t2|t1f|t1p)", role ? role : "");
        return AIRY_ERR_INVALID_PARAM;
    }
    const char *m = model ? model : "";
    if (m[0] && !slots_id_ok(m)) {
        snprintf(err, cap, "invalid model id (empty/too long/quote/control char)");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&svc->lock);
    int hit = !m[0] || provider_registry_find(svc->registry, m) != NULL;
    svc_model_slots_t next = svc->slots;
    airy_mtx_unlock(&svc->lock);
    if (!hit) {
        char names[1024] = {0};
        slot_names_t sn = {names, sizeof(names), 0};
        airy_mtx_lock(&svc->lock);
        provider_registry_enumerate(svc->registry, slots_name_cb, &sn);
        airy_mtx_unlock(&svc->lock);
        snprintf(err, cap, "model '%s' not in registry; available: %s", m,
                 names[0] ? names : "(none)");
        return AIRY_ERR_INVALID_PARAM;
    }
    AIRY_STRNCPY_TERM(svc_slot_buf(&next, (unsigned)r), m, SVC_MODEL_ID_MAX);

    char path[1024];
    if (slots_target(svc, path, sizeof(path)) != 0) {
        snprintf(err, cap, "no writable model.yaml (config dir unavailable)");
        return AIRY_ERR_IO;
    }
    int rc = svc_slot_save(path, &next);
    if (rc != 0) {
        snprintf(err, cap, "write %s failed rc=%d", path, rc);
        return AIRY_ERR_IO;
    }
    /* 落盘后立刻走同一条重载链，使服务内视图与文件一致；失败则如实报错，
     * 旧配置继续服务（文件已写入，下一次 SIGHUP 仍可收敛）。 */
    rc = llm_service_reload(svc);
    if (rc != AIRY_OK) {
        snprintf(err, cap, "saved to %s but reload rejected rc=%d", path, rc);
        return AIRY_ERR_STATE_ERROR;
    }
    return 0;
}

const char *llm_service_default_model(const llm_service_t *svc)
{
    if (!svc)
        return NULL;
    return svc->default_model[0] ? svc->default_model : NULL;
}
