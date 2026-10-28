// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_reload.c
 * @brief 配置热重载执行器（0.1.20 D2-10）。
 *
 * 重载切成三段，只有中段需要冻结在途请求：
 *   读入 —— 全部读进暂存态，任何失败都不触碰在线配置
 *   排空 —— 拒绝新请求，等待在途请求归还（或超时强制）
 *   换表 —— 一次性替换注册表/价格/路由/默认值，世代 +1
 *
 * 单飞由 svc_swap 的 draining 位保证：第二次进入直接判忙并丢弃已读入的
 * 暂存态，不产生半成品。reload_lock 只保护退避窗口，并在排空前释放——
 * 排空期间新请求应立即被判忙，而不是排队等锁。
 *
 * 换表阶段没有可失败的操作，因此排空一旦开始就必然以 commit 收尾；失败
 * 只可能发生在读入阶段，此时在线配置毫发无损。
 */

#include "airy_memory.h"
#include "error.h"
#include "platform_misc.h"
#include "providers/core/registry.h"
#include "rpc/service.h"
#include "rpc/service_boot.h"
#include "svc_logger.h"

#include "config/config_internal.h"
#include "router/core/llm_router.h"
#include "rpc/rpc_internal.h"

/* 退休表：排空超时后旧装载仍有请求持有其句柄，按宽限期延后回收。
 * 槽位满说明连续多次排空超时，此时强制回收最旧一档并留证，避免无界增长。 */
static void retire_reap(llm_service_t *svc, uint64_t now)
{
    int keep = 0;
    for (int i = 0; i < svc->retired_n; i++) {
        llm_retired_t *r = &svc->retired[i];
        if (now - r->retire_ms < LLM_RETIRED_GRACE_MS) {
            if (keep != i)
                svc->retired[keep] = *r;
            keep++;
            continue;
        }
        provider_registry_destroy(r->registry);
        free_pricing_rules(r->rules, r->rule_count);
        SVC_LOG_INFO("C-L02: SVC: retired config released after grace");
    }
    svc->retired_n = keep;
}

static void retire_push(llm_service_t *svc, provider_registry_t *reg, void *rules, int rule_n)
{
    uint64_t now = airy_time_ms();
    retire_reap(svc, now);

    if (svc->retired_n >= LLM_RETIRED_MAX) {
        provider_registry_destroy(svc->retired[0].registry);
        free_pricing_rules(svc->retired[0].rules, svc->retired[0].rule_count);
        for (int i = 1; i < LLM_RETIRED_MAX; i++)
            svc->retired[i - 1] = svc->retired[i];
        svc->retired_n = LLM_RETIRED_MAX - 1;
        SVC_LOG_WARN("C-L02: SVC: retired table full — oldest config force-released");
    }

    llm_retired_t *slot = &svc->retired[svc->retired_n];
    slot->registry = reg;
    slot->rules = (pricing_rule_t *)rules;
    slot->rule_count = rule_n;
    slot->retire_ms = now;
    svc->retired_n++;
}

/* 旧装载退场：正常排空下已无持有者，可立即释放；强制排空下必须延后。 */
static void old_dispose(llm_service_t *svc, provider_registry_t *reg, void *rules, int rule_n,
                        int forced)
{
    if (forced) {
        retire_push(svc, reg, rules, rule_n);
        SVC_LOG_WARN("C-L02: SVC: drain timed out — old config retired for %ums",
                     (unsigned)LLM_RETIRED_GRACE_MS);
        return;
    }
    provider_registry_destroy(reg);
    free_pricing_rules((pricing_rule_t *)rules, rule_n);
}

int llm_service_reload(llm_service_t *svc)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;

    uint64_t now = airy_time_ms();
    airy_mtx_lock(&svc->reload_lock);
    if (!svc->cfg_path[0] || (svc->reload_next_ms != 0U && now < svc->reload_next_ms)) {
        airy_mtx_unlock(&svc->reload_lock);
        return 1;
    }

    /* 读入段：失败即退避，旧配置继续服务。 */
    llm_boot_t b;
    llm_boot_fill(&b, svc->cfg_path);
    provider_registry_t *nreg = b.load_err ? NULL : provider_registry_create(&b.base);
    if (!nreg) {
        llm_boot_drop(&b);
        svc->reload_fail++;
        svc->reload_next_ms = now + LLM_RELOAD_BACKOFF_MS;
        airy_mtx_unlock(&svc->reload_lock);
        SVC_LOG_ERROR("C-L02: SVC: reload aborted (config unreadable or registry build failed), "
                      "current config kept for %ums",
                      (unsigned)LLM_RELOAD_BACKOFF_MS);
        return AIRY_ERR_STATE_ERROR;
    }

    /* 快照不在此处重照（0.1.20 D2-7）：应用成功后由 svc_mw_reload 自行
     * rebase 主监视，apply 回调再 rebase 副监视。此处照快照等于同一文件
     * 读两遍，且留下提交前被再次改写的 TOCTOU 窗口。 */

    airy_mtx_unlock(&svc->reload_lock);

    /* 排空段：这段时间新请求一律判忙，不排队。 */
    int forced = svc_swap_begin(&svc->swap);
    if (forced < 0) {
        provider_registry_destroy(nreg);
        llm_boot_drop(&b);
        return 1;
    }

    /* 换表段：注册表指针须在 svc->lock 下替换（svc_find_prov 同锁取用），
     * 其余字段只在排空窗口内被读写。 */
    airy_mtx_lock(&svc->lock);
    provider_registry_t *oreg = svc->registry;
    svc->registry = nreg;
    airy_mtx_unlock(&svc->lock);

    void *orules = svc->rules;
    int orule_n = (int)svc->rule_count;
    size_t nprov = b.prov_n;
    llm_boot_apply(svc, &b);
    /* provider 数组已被注册表深拷贝，规则表所有权已转 svc。 */
    llm_boot_drop(&b);

    if (cost_set_rules(svc->cost, (const pricing_rule_t *)svc->rules,
                               (int)svc->rule_count) != 0)
        SVC_LOG_WARN("C-L02: SVC: pricing table swap failed — cost keeps previous prices");

    /* 端点表按新注册表整表重建：追加式注册会把已下线模型永久留在表里。 */
    if (llm_rtr_reset_eps() == 0)
        register_router_endpoints(svc);

    if (svc_swap_commit(&svc->swap) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: swap commit rejected — drain state lost");
        svc_swap_abort(&svc->swap);
    }

    old_dispose(svc, oreg, orules, orule_n, forced == 1);

    uint32_t gen = 0;
    svc_swap_stat(&svc->swap, &gen, NULL, NULL, NULL, NULL);
    svc->reload_ok++;
    svc->reload_next_ms = 0;
    SVC_LOG_INFO("C-L02: SVC: reload done gen=%u forced=%d providers=%zu rules=%zu", gen, forced,
                 nprov, svc->rule_count);
    return AIRY_OK;
}
