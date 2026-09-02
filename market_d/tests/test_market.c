/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file test_market.c
 * @brief market_service 公开 API 单元测试（内存域）。
 * @details 覆盖生命周期、agent/skill 注册与同名更新、搜索、
 *          已安装列表、更新检查与 NULL 参数防线。
 *          install/uninstall 涉及落盘与网络，由 e2e 回归覆盖，此处不涉及。
 */

#include "market_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return -1;                                                   \
        }                                                                \
    } while (0)

static int has_agent_id(agent_info_t **items, size_t count, const char *id)
{
    for (size_t i = 0; i < count; i++) {
        if (items[i] && items[i]->agent_id && strcmp(items[i]->agent_id, id) == 0)
            return 1;
    }
    return 0;
}

static int test_lifecycle(void)
{
    market_service_t *svc = NULL;

    CHECK(market_service_create(NULL, NULL) != 0);
    CHECK(market_service_create(NULL, &svc) == 0);
    CHECK(svc != NULL);
    CHECK(market_service_destroy(svc) == 0);
    CHECK(market_service_destroy(NULL) != 0);

    market_config_t cfg = {.registry_url = "http://registry.local",
                           .storage_path = NULL,
                           .sync_interval_ms = 30000,
                           .cache_ttl_ms = 300000,
                           .enable_remote_registry = false,
                           .enable_auto_update = false};
    svc = NULL;
    CHECK(market_service_create(&cfg, &svc) == 0);
    CHECK(market_service_destroy(svc) == 0);
    return 0;
}

static int test_agent_registry(void)
{
    market_service_t *svc = NULL;
    CHECK(market_service_create(NULL, &svc) == 0);

    agent_info_t agent = {0};
    agent.agent_id = (char *)"agent-001";
    agent.name = (char *)"assistant-core";
    agent.version = (char *)"1.0.0";
    agent.description = (char *)"general assistant agent";
    agent.type = AGENT_TYPE_ASSISTANT;
    agent.status = AGENT_STATUS_AVAILABLE;
    agent.author = (char *)"SPHARX";
    agent.rating = 4.5f;
    agent.download_count = 100;

    CHECK(market_service_register_agent(NULL, &agent) != 0);
    CHECK(market_service_register_agent(svc, NULL) != 0);
    CHECK(market_service_register_agent(svc, &agent) == 0);

    agent_info_t second = agent;
    second.agent_id = (char *)"agent-002";
    second.name = (char *)"expert-math";
    second.description = (char *)"math expert agent";
    second.type = AGENT_TYPE_EXPERT;
    second.status = AGENT_STATUS_DISABLED;
    CHECK(market_service_register_agent(svc, &second) == 0);

    search_params_t sp = {0};
    sp.query = (char *)"assistant";
    sp.limit = 10;
    agent_info_t **found = NULL;
    size_t count = 0;
    CHECK(market_service_search_agents(svc, &sp, &found, &count) == 0);
    CHECK(count == 1);
    CHECK(found[0]->version && strcmp(found[0]->version, "1.0.0") == 0);
    free(found);

    /* 同名更新：agent_id 命中即就地覆盖，不增长条目数 */
    agent.version = (char *)"1.0.1";
    CHECK(market_service_register_agent(svc, &agent) == 0);
    sp.query = NULL;
    found = NULL;
    count = 0;
    CHECK(market_service_search_agents(svc, &sp, &found, &count) == 0);
    CHECK(count == 2);
    CHECK(strcmp(found[0]->version, "1.0.1") == 0);
    free(found);

    sp.query = (char *)"no-such-keyword";
    found = NULL;
    count = 99;
    CHECK(market_service_search_agents(svc, &sp, &found, &count) == 0);
    CHECK(count == 0);
    free(found);

    /* 已安装列表仅收录 AVAILABLE/ERROR 状态 */
    agent_info_t **listed = NULL;
    count = 0;
    CHECK(market_service_get_installed_agents(svc, &listed, &count) == 0);
    CHECK(count == 1);
    CHECK(strcmp(listed[0]->agent_id, "agent-001") == 0);
    free(listed);

    CHECK(market_service_destroy(svc) == 0);
    return 0;
}

static int test_skill_registry(void)
{
    market_service_t *svc = NULL;
    CHECK(market_service_create(NULL, &svc) == 0);

    skill_info_t skill = {0};
    skill.skill_id = (char *)"skill-001";
    skill.name = (char *)"calendar-tool";
    skill.version = (char *)"2.1.0";
    skill.description = (char *)"calendar integration skill";
    skill.type = SKILL_TYPE_TOOL;
    skill.author = (char *)"SPHARX";

    CHECK(market_service_register_skill(svc, &skill) == 0);
    skill.version = (char *)"2.2.0";
    CHECK(market_service_register_skill(svc, &skill) == 0);

    search_params_t sp = {0};
    sp.query = (char *)"calendar";
    sp.limit = 10;
    skill_info_t **found = NULL;
    size_t count = 0;
    CHECK(market_service_search_skills(svc, &sp, &found, &count) == 0);
    CHECK(count == 1);
    CHECK(found[0]->version && strcmp(found[0]->version, "2.2.0") == 0);
    free(found);

    skill_info_t **listed = NULL;
    count = 0;
    CHECK(market_service_get_installed_skills(svc, &listed, &count) == 0);
    CHECK(count == 1);
    free(listed);

    CHECK(market_service_destroy(svc) == 0);
    return 0;
}

static int test_check_update(void)
{
    market_service_t *svc = NULL;
    CHECK(market_service_create(NULL, &svc) == 0);

    agent_info_t agent = {0};
    agent.agent_id = (char *)"agent-100";
    agent.name = (char *)"updatable";
    agent.version = (char *)"3.0.0";
    agent.status = AGENT_STATUS_AVAILABLE;
    CHECK(market_service_register_agent(svc, &agent) == 0);

    bool has_update = true;
    char *latest = NULL;
    CHECK(market_service_check_update(svc, "agent-100", &has_update, &latest) == 0);
    CHECK(has_update == false);
    CHECK(latest && strcmp(latest, "3.0.0") == 0);
    free(latest);

    latest = (char *)0x1;
    CHECK(market_service_check_update(svc, "missing-id", &has_update, &latest) != 0);
    CHECK(latest == NULL);

    CHECK(market_service_check_update(NULL, "agent-100", &has_update, &latest) != 0);
    CHECK(market_service_destroy(svc) == 0);
    return 0;
}

static int test_search_limit(void)
{
    market_service_t *svc = NULL;
    CHECK(market_service_create(NULL, &svc) == 0);

    char *ids[] = {(char *)"agent-200", (char *)"agent-201", (char *)"agent-202"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        agent_info_t agent = {0};
        agent.agent_id = ids[i];
        agent.name = ids[i];
        agent.version = (char *)"1.0.0";
        agent.status = AGENT_STATUS_AVAILABLE;
        CHECK(market_service_register_agent(svc, &agent) == 0);
    }

    search_params_t sp = {0};
    sp.limit = 2;
    agent_info_t **found = NULL;
    size_t count = 0;
    CHECK(market_service_search_agents(svc, &sp, &found, &count) == 0);
    CHECK(count == 2);
    free(found);

    sp.limit = 0;
    found = NULL;
    count = 0;
    CHECK(market_service_search_agents(svc, &sp, &found, &count) == 0);
    CHECK(count == 3);
    CHECK(has_agent_id(found, count, "agent-202"));
    free(found);

    CHECK(market_service_destroy(svc) == 0);
    return 0;
}

typedef int (*test_fn)(void);

int main(void)
{
    static const test_fn tests[] = {test_lifecycle, test_agent_registry, test_skill_registry,
                                    test_check_update, test_search_limit};
    const size_t n = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("market_service unit tests\n");
    for (size_t i = 0; i < n; i++) {
        int ret = tests[i]();
        printf("[%d/%zu] %s\n", (int)i + 1, n, ret == 0 ? "ok" : "FAILED");
        if (ret == 0)
            passed++;
    }

    printf("%zu/%zu passed\n", passed, n);
    return passed == n ? 0 : 1;
}
