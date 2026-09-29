// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_rpc.c
 * @brief market.* RPC 方法域：注册（agent/skill）/ 搜索 / 安装 /
 *        发布 / 健康检查 / 服务统计。
 *
 * 由 main.c 按单一职责拆分（0.1.19 gen5 装配）：method_fn 薄壳 m_* 把
 * 机制层注入的 &client_fd 翻译为协议无关的 handler（cJSON*, int,
 * airy_sock_t）。L2 协议别名 search->search_agents、install->
 * install_agent；publish 复用 install 逻辑。
 */

#include "airy_memory.h"
#include "error.h"
#include "market_d_internal.h"
#include "svc_market_d.h"

#include "jsonrpc_helpers.h"
#include "param_validator.h"
#include "svc_logger.h"

#include <time.h>

static void handle_register_agent(cJSON *params, int id, airy_sock_t fd);
static void handle_search_agents(cJSON *params, int id, airy_sock_t fd);
static void handle_install_agent(cJSON *params, int id, airy_sock_t fd);
static void handle_register_skill(cJSON *params, int id, airy_sock_t fd);
static void handle_search_skills(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_publish(cJSON *params, int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);

void m_register_agent(cJSON *params, int id, void *user_data)
{
    handle_register_agent(params, id, *(airy_sock_t *)user_data);
}

void m_search_agents(cJSON *params, int id, void *user_data)
{
    handle_search_agents(params, id, *(airy_sock_t *)user_data);
}

void m_install_agent(cJSON *params, int id, void *user_data)
{
    handle_install_agent(params, id, *(airy_sock_t *)user_data);
}

void m_register_skill(cJSON *params, int id, void *user_data)
{
    handle_register_skill(params, id, *(airy_sock_t *)user_data);
}

void m_search_skills(cJSON *params, int id, void *user_data)
{
    handle_search_skills(params, id, *(airy_sock_t *)user_data);
}

void m_publish(cJSON *params, int id, void *user_data)
{
    handle_publish(params, id, *(airy_sock_t *)user_data);
}

/* L2 协议别名：market.search == search_agents；market.install ==
 * install_agent（02-l2-service-protocol.md）。 */
void m_search(cJSON *params, int id, void *user_data)
{
    handle_search_agents(params, id, *(airy_sock_t *)user_data);
}

void m_install(cJSON *params, int id, void *user_data)
{
    handle_install_agent(params, id, *(airy_sock_t *)user_data);
}

void m_health_check(cJSON *params, int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

void m_get_stats(cJSON *params, int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_json = jsonrpc_get_object_param(params, "agent");
    if (!agent_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent object", id);
        return;
    }

    agent_info_t info = {0};
    const char *aid = get_string_field(agent_json, "agent_id", NULL);
    if (!aid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }
    info.agent_id = (char *)aid;

    info.name = (char *)get_string_field(agent_json, "name", NULL);
    info.version = (char *)get_string_field(agent_json, "version", NULL);
    info.description = (char *)get_string_field(agent_json, "description", NULL);
    info.author = (char *)get_string_field(agent_json, "author", NULL);

    int ret = market_service_register_agent(g_service, &info);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Register failed", id);
        SVC_LOG_ERROR("Failed to register agent: %s (error=%d)", aid, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "registered");
        cJSON_AddStringToObject(result, "agent_id", aid);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("Agent registered: %s v%s", aid, info.version ? info.version : "unknown");
    }
}

static void handle_search_agents(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *keyword = get_string_field(params, "keyword", "");
    size_t offset = (size_t)get_double_field(params, "offset", 0.0);
    size_t limit = (size_t)get_double_field(params, "limit", 20.0);

    agent_info_t **agents = NULL;
    size_t count = 0;

    search_params_t sp;
    AIRY_MEMSET(&sp, 0, sizeof(sp));
    sp.query = (char *)keyword;
    sp.limit = limit;
    sp.offset = offset;

    int ret = market_service_search_agents(g_service, &sp, &agents, &count);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Search failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count && agents && agents[i]; i++) {
        cJSON *a = cJSON_CreateObject();
        if (agents[i]->agent_id)
            cJSON_AddStringToObject(a, "agent_id", agents[i]->agent_id);
        if (agents[i]->name)
            cJSON_AddStringToObject(a, "name", agents[i]->name);
        if (agents[i]->version)
            cJSON_AddStringToObject(a, "version", agents[i]->version);
        if (agents[i]->description)
            cJSON_AddStringToObject(a, "description", agents[i]->description);
        if (agents[i]->author)
            cJSON_AddStringToObject(a, "author", agents[i]->author);
        cJSON_AddBoolToObject(a, "installed", agents[i]->status == AGENT_STATUS_AVAILABLE);
        cJSON_AddItemToArray(arr, a);
    }
    AIRY_FREE(agents);

    JSONRPC_SEND_SUCCESS(client_fd, arr, id);
}

static void handle_install_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *aid = get_string_field(params, "agent_id", NULL);
    if (!aid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    const char *version = get_string_field(params, "version", "latest");
    const char *install_path = get_string_field(params, "install_path", NULL);
    cJSON *force_json = cJSON_GetObjectItem(params, "force_update");
    bool force = cJSON_IsTrue(force_json);

    install_request_t req;
    AIRY_MEMSET(&req, 0, sizeof(req));
    req.id = (char *)aid;
    req.version = (char *)version;
    req.install_path = (char *)install_path;
    req.force_update = force;

    install_result_t *result = NULL;
    int ret = market_service_install_agent(g_service, &req, &result);

    if (ret != AIRY_SUCCESS || !result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Install failed", id);
        SVC_LOG_ERROR("Failed to install agent: %s@%s (error=%d)", aid, version, ret);
    } else {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", result->success ? "installed" : "failed");
        cJSON_AddStringToObject(resp, "agent_id", aid);
        cJSON_AddStringToObject(resp, "installed_version",
                                result->installed_version ? result->installed_version : version);
        if (result->message) {
            cJSON_AddStringToObject(resp, "message", result->message);
        }
        if (result->install_path) {
            cJSON_AddStringToObject(resp, "install_path", result->install_path);
        }
        JSONRPC_SEND_SUCCESS(client_fd, resp, id);
        SVC_LOG_INFO("Agent install %s: %s@%s (ret=%d)", result->success ? "OK" : "FAILED", aid,
                     version, ret);
    }

    if (result) {
        AIRY_FREE(result->message);
        AIRY_FREE(result->installed_version);
        AIRY_FREE(result->install_path);
        AIRY_FREE(result);
    }
}

static void handle_register_skill(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *skill_json = jsonrpc_get_object_param(params, "skill");
    if (!skill_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing skill object", id);
        return;
    }

    skill_info_t info = {0};
    const char *sid = get_string_field(skill_json, "skill_id", NULL);
    if (!sid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing skill_id", id);
        return;
    }
    info.skill_id = (char *)sid;

    info.name = (char *)get_string_field(skill_json, "name", NULL);
    info.version = (char *)get_string_field(skill_json, "version", NULL);

    int ret = market_service_register_skill(g_service, &info);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Register failed", id);
        SVC_LOG_ERROR("Failed to register skill: %s (error=%d)", sid, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "registered");
        cJSON_AddStringToObject(result, "skill_id", sid);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("Skill registered: %s", sid);
    }
}

static void handle_search_skills(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *keyword = get_string_field(params, "keyword", "");

    skill_info_t **skills = NULL;
    size_t count = 0;

    search_params_t sp;
    AIRY_MEMSET(&sp, 0, sizeof(sp));
    sp.query = (char *)keyword;
    sp.limit = 20;
    sp.offset = 0;

    int ret = market_service_search_skills(g_service, &sp, &skills, &count);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Search failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count && skills && skills[i]; i++) {
        cJSON *s = cJSON_CreateObject();
        if (skills[i]->skill_id)
            cJSON_AddStringToObject(s, "skill_id", skills[i]->skill_id);
        if (skills[i]->name)
            cJSON_AddStringToObject(s, "name", skills[i]->name);
        if (skills[i]->version)
            cJSON_AddStringToObject(s, "version", skills[i]->version);
        if (skills[i]->description)
            cJSON_AddStringToObject(s, "description", skills[i]->description);
        cJSON_AddItemToArray(arr, s);
    }
    AIRY_FREE(skills);

    JSONRPC_SEND_SUCCESS(client_fd, arr, id);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "market_d");
    cJSON_AddBoolToObject(result, "healthy", true);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "market_d");
    if (g_service) {
        search_params_t sp;
        AIRY_MEMSET(&sp, 0, sizeof(sp));
        sp.query = (char *)"";
        sp.limit = 100000;
        sp.offset = 0;
        agent_info_t **agents = NULL;
        size_t agent_count = 0;
        if (market_service_search_agents(g_service, &sp, &agents, &agent_count) == AIRY_SUCCESS) {
            cJSON_AddNumberToObject(result, "agents", (double)agent_count);
            if (agents)
                AIRY_FREE(agents);
        }
        skill_info_t **skills = NULL;
        size_t skill_count = 0;
        if (market_service_search_skills(g_service, &sp, &skills, &skill_count) == AIRY_SUCCESS) {
            cJSON_AddNumberToObject(result, "skills", (double)skill_count);
            if (skills)
                AIRY_FREE(skills);
        }

        agent_info_t **installed_agents = NULL;
        size_t installed_agent_count = 0;
        if (market_service_get_installed_agents(g_service, &installed_agents,
                                                &installed_agent_count) == AIRY_SUCCESS) {
            cJSON_AddNumberToObject(result, "installed_agents", (double)installed_agent_count);
            if (installed_agents)
                AIRY_FREE(installed_agents);
        }
        skill_info_t **installed_skills = NULL;
        size_t installed_skill_count = 0;
        if (market_service_get_installed_skills(g_service, &installed_skills,
                                                &installed_skill_count) == AIRY_SUCCESS) {
            cJSON_AddNumberToObject(result, "installed_skills", (double)installed_skill_count);
            if (installed_skills)
                AIRY_FREE(installed_skills);
        }
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/*
 * L2 standard method market.publish (02-l2-service-protocol.md):
 * reuses the existing install logic, accepts a params.agent or
 * params.skill object, persists to $AIRY_HOME/agents or $AIRY_HOME/skills
 * and returns the install path.
 */
static void handle_publish(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_json = jsonrpc_get_object_param(params, "agent");
    cJSON *skill_json = jsonrpc_get_object_param(params, "skill");

    if (!agent_json && !skill_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent or skill object", id);
        return;
    }

    install_request_t req;
    AIRY_MEMSET(&req, 0, sizeof(req));
    req.force_update = get_bool_field(params, "force_update", false);
    req.install_path = (char *)get_string_field(params, "install_path", NULL);

    bool is_agent = agent_json != NULL;
    const char *id_str = NULL;
    const char *version = "latest";

    if (is_agent) {
        id_str = get_string_field(agent_json, "agent_id", NULL);
        version = get_string_field(agent_json, "version", "latest");
    } else {
        id_str = get_string_field(skill_json, "skill_id", NULL);
        version = get_string_field(skill_json, "version", "latest");
    }
    if (!id_str) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           is_agent ? "Missing agent_id" : "Missing skill_id", id);
        return;
    }
    req.id = (char *)id_str;
    req.version = (char *)version;

    install_result_t *result = NULL;
    int ret = is_agent ? market_service_install_agent(g_service, &req, &result) :
                         market_service_install_skill(g_service, &req, &result);

    if (ret != AIRY_SUCCESS || !result || !result->success) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Publish failed", id);
        SVC_LOG_ERROR("market.publish failed: %s=%s@%s (error=%d)", is_agent ? "agent" : "skill",
                      id_str, version, ret);
    } else {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "published");
        cJSON_AddStringToObject(resp, "type", is_agent ? "agent" : "skill");
        cJSON_AddStringToObject(resp, "id", id_str);
        cJSON_AddStringToObject(resp, "published_version",
                                result->installed_version ? result->installed_version : version);
        if (result->message)
            cJSON_AddStringToObject(resp, "message", result->message);
        if (result->install_path)
            cJSON_AddStringToObject(resp, "install_path", result->install_path);
        JSONRPC_SEND_SUCCESS(client_fd, resp, id);
        SVC_LOG_INFO("market.publish OK: %s=%s@%s path=%s", is_agent ? "agent" : "skill", id_str,
                     version, result->install_path ? result->install_path : "?");
    }

    if (result) {
        AIRY_FREE(result->message);
        AIRY_FREE(result->installed_version);
        AIRY_FREE(result->install_path);
        AIRY_FREE(result);
    }
}
