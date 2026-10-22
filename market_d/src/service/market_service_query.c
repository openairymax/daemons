// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_query.c
 * @brief Market 只读查询域：关键字检索 + 已安装清单 + 更新检查。
 *
 * 检索与清单两族函数共享同一套「判定 -> 倍增收集」骨架，本文件以
 * scan() 单点承载该骨架，四族入口只提供判定谓词与文案；全部函数仅持
 * service->lock 读快照，不改变服务状态。
 */

#include "airy_memory.h"
#include "error.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

#include "market_service_internal.h"

typedef bool (*mk_pick_t)(const void *item, const char *q);

enum { MK_INIT_CAP = 16 };

/* 关键字命中：空查询放行，否则在 id / name / description 任一命中。 */
static bool pick_text(const char *id, const char *name, const char *desc, const char *q)
{
    if (!q || !q[0])
        return true;
    return strstr(id, q) || strstr(name, q) || (desc && strstr(desc, q));
}

static bool pick_agent(const void *item, const char *q)
{
    const agent_info_t *a = (const agent_info_t *)item;

    return pick_text(a->agent_id, a->name, a->description, q);
}

static bool pick_skill(const void *item, const char *q)
{
    const skill_info_t *s = (const skill_info_t *)item;

    return pick_text(s->skill_id, s->name, s->description, q);
}

/* 已安装：可用或错误态的都仍需向调用方暴露，便于上层展示与重试。 */
static bool pick_live(const void *item, const char *q)
{
    const agent_info_t *a = (const agent_info_t *)item;

    (void)q;
    return a->status == AGENT_STATUS_AVAILABLE || a->status == AGENT_STATUS_ERROR;
}

static bool pick_all(const void *item, const char *q)
{
    (void)item;
    (void)q;
    return true;
}

/**
 * @brief 按谓词收集快照，结果数组倍增扩容。
 *
 * 倍增溢出视为容量上限，返回已收集部分；分配失败则释放、解锁并上报，
 * 由本函数直接返回负错误码。
 */
static int scan(market_service_t *svc, bool skills, mk_pick_t pick,
                const search_params_t *params, const char *fn, const char *alloc_msg,
                const char *resize_msg, void ***out, size_t *n)
{
    const void *const *items;
    size_t total, cap = MK_INIT_CAP, found = 0;
    void **res = (void **)AIRY_MALLOC(sizeof(*res) * cap);

    if (!res) {
        SVC_LOG_ERROR("%s: allocation failed (%s)", fn, alloc_msg);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, alloc_msg);
    }

    airy_mtx_lock(&svc->lock);
    items = skills ? (const void *const *)svc->skills : (const void *const *)svc->agents;
    total = skills ? svc->skill_count : svc->agent_count;

    for (size_t i = 0; i < total; i++) {
        void **tmp;

        if (!pick(items[i], params ? params->query : NULL))
            continue;
        if (found >= cap) {
            if (cap > SIZE_MAX / (2 * sizeof(*res)))
                break;
            tmp = (void **)AIRY_REALLOC(res, 2 * cap * sizeof(*res));
            if (!tmp) {
                SVC_LOG_ERROR("%s: resize failed at %zu slots (%s)", fn, cap, resize_msg);
                AIRY_FREE(res);
                airy_mtx_unlock(&svc->lock);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, resize_msg);
            }
            res = tmp;
            cap *= 2;
        }
        res[found++] = (void *)items[i];
        if (params && params->limit > 0 && found >= params->limit)
            break;
    }
    airy_mtx_unlock(&svc->lock);

    *out = res;
    *n = found;
    return 0;
}

int market_service_search_agents(market_service_t *service, const search_params_t *params,
                                 agent_info_t ***agents, size_t *count)
{
    void **arr = NULL;
    int rc;

    if (!service || !params || !agents || !count || !service->initialized) {
        SVC_LOG_ERROR("market_service_search_agents: NULL parameter or not initialized "
                      "(service=%p, params=%p, agents=%p, count=%p, initialized=%d)",
                      (const void *)service, (const void *)params, (const void *)agents,
                      (const void *)count, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    rc = scan(service, false, pick_agent, params, "market_service_search_agents",
              "failed to allocate search results", "failed to resize search results", &arr,
              count);
    if (rc < 0)
        return rc;
    *agents = (agent_info_t **)arr;
    return 0;
}

int market_service_search_skills(market_service_t *service, const search_params_t *params,
                                 skill_info_t ***skills, size_t *count)
{
    void **arr = NULL;
    int rc;

    if (!service || !params || !skills || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    rc = scan(service, true, pick_skill, params, "market_service_search_skills",
              "failed to allocate skill search results", "failed to resize skill search results",
              &arr, count);
    if (rc < 0)
        return rc;
    *skills = (skill_info_t **)arr;
    return 0;
}

int market_service_get_installed_agents(market_service_t *service, agent_info_t ***agents,
                                        size_t *count)
{
    void **arr = NULL;
    int rc;

    if (!service || !agents || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    rc = scan(service, false, pick_live, NULL, "market_service_get_installed_agents",
              "failed to allocate installed agents list", "failed to resize installed agents list",
              &arr, count);
    if (rc < 0)
        return rc;
    *agents = (agent_info_t **)arr;
    return 0;
}

int market_service_get_installed_skills(market_service_t *service, skill_info_t ***skills,
                                        size_t *count)
{
    void **arr = NULL;
    int rc;

    if (!service || !skills || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    rc = scan(service, true, pick_all, NULL, "market_service_get_installed_skills",
              "failed to allocate installed skills list", "failed to resize installed skills list",
              &arr, count);
    if (rc < 0)
        return rc;
    *skills = (skill_info_t **)arr;
    return 0;
}

int market_service_check_update(market_service_t *service, const char *id, bool *has_update,
                                char **latest_version)
{
    if (!service || !id || !has_update || !latest_version || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    *has_update = false;

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, id) == 0) {
            *latest_version = AIRY_STRDUP(service->agents[i]->version);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    for (size_t i = 0; i < service->skill_count; i++) {
        if (strcmp(service->skills[i]->skill_id, id) == 0) {
            *latest_version = AIRY_STRDUP(service->skills[i]->version);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    *latest_version = NULL;
    airy_mtx_unlock(&service->lock);
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "update check: id not found");
}
