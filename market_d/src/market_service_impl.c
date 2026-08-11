// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file market_service_impl.c
 * @brief 市场服务核心实现
 * @details 定义 struct market_service 并实现 market_service.h 中的所有公共API
 */

#include "daemon_errors.h"
#include "market_service.h"
#include "platform.h"
#include "svc_logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef _WIN32
#include <ftw.h>
#include <unistd.h>
#include <sys/wait.h>
#else
#include <windows.h>
#endif

#include <airymax/sched.h>

#define MAX_SKILLS 256

#ifdef _WIN32
/**
 * @brief Windows 等价的同步外部命令执行（替代 POSIX fork/execlp/waitpid）。
 *
 * 语义对齐原 curl/tar 调用：
 *   - 不经过 shell：lpApplicationName=NULL，命令行首 token 为可执行文件名，
 *     CreateProcess 按 PATH 搜索（等价 execlp），无命令注入风险；
 *   - 输出继承父进程控制台句柄（对齐 POSIX 版本未重定向的行为）；
 *   - 阻塞等待子进程退出并返回退出码；CreateProcess 失败返回 -1
 *     （等价 POSIX fork 失败 / execlp 失败 _exit(127) 的非零语义）。
 *
 * 局限：Windows 命令行经 CreateProcess 重新解析为 argv，含空格的参数已加
 * 双引号转义；本上下文参数为受控的 URL/路径/flag，可安全处理。
 */
static int win_run_command(const char *prog, const char *const args[])
{
    char cmdline[2048];
    size_t off = 0;
    int n = snprintf(cmdline, sizeof(cmdline), "\"%s\"", prog);
    if (n < 0)
        return AIRY_ERR_FAIL;
    off = (size_t)n;
    for (size_t i = 0; args && args[i] && off < sizeof(cmdline) - 1; i++) {
        const char *a = args[i];
        int quote = (strpbrk(a, " \t\"") != NULL);
        n = quote ? snprintf(cmdline + off, sizeof(cmdline) - off, " \"%s\"", a) :
                    snprintf(cmdline + off, sizeof(cmdline) - off, " %s", a);
        if (n < 0)
            break;
        off += (size_t)n;
    }
    cmdline[sizeof(cmdline) - 1] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return AIRY_ERR_EXEC_FAIL;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#endif /* _WIN32 */

#ifndef _WIN32
static int nftw_remove_cb(const char *fpath, const struct stat *sb, int typeflag,
                          struct FTW *ftwbuf)
{
    (void)sb;
    (void)ftwbuf;
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        return rmdir(fpath);
    }
    return remove(fpath);
}
#endif

/**
 * @brief 递归删除目录树（跨平台）
 *
 * POSIX: 使用 nftw 深度优先遍历删除。
 * Windows: 使用 FindFirstFile/FindNextFile 递归下降删除，
 *          清除只读属性后删除文件，最后 RemoveDirectory 删除空目录。
 *          行为对齐 POSIX 版本，无 shell 调用，无命令注入风险。
 */
static int recursive_remove(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return AIRY_ERR_NOT_FOUND;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {

        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) ? 0 : -1;
    }

    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryA(path) ? 0 : -1;
    }
    do {
        if (fd.cFileName[0] == '.')
            continue;
        char child[MAX_PATH];
        snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            recursive_remove(child);
        } else {
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    return nftw(path, nftw_remove_cb, 64, FTW_DEPTH | FTW_PHYS);
#endif
}

static int is_safe_for_shell(const char *str)
{
    if (!str)
        return 0;
    const char *dangerous = ";|&$`'\"\\(){}[]!#~<>\n\r";
    for (size_t i = 0; i < strlen(dangerous); i++) {
        if (strchr(str, dangerous[i]))
            return 0;
    }
    return 1;
}

static int is_valid_url(const char *url)
{
    if (!url || strlen(url) == 0)
        return 0;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return 0;
    return is_safe_for_shell(url);
}

static int is_safe_path_component(const char *str)
{
    if (!str || strlen(str) == 0)
        return 0;
    if (strchr(str, '/') || strchr(str, '\\'))
        return 0;
    if (strstr(str, ".."))
        return 0;
    return 1;
}

/* 默认存储根目录：$AIRY_HOME/agents（或 /skills）。
 * 历史上 install/uninstall 硬编码 "./agents" 相对路径，随进程 CWD 漂移，
 * 部署到 $AIRY_HOME/bin 后必然落错位置；统一收敛到 AIRY_HOME 路径体系。 */
static const char *market_default_storage(const char *subdir)
{
    static __thread char s_path[AIRY_PATH_MAX];
    snprintf(s_path, sizeof(s_path), "%s/%s", airy_home_dir(), subdir);
    return s_path;
}

struct market_service {
    market_config_t config;
    agent_info_t *agents[AIRY_CAP_MAX_AGENTS];
    size_t agent_count;
    skill_info_t *skills[MAX_SKILLS];
    size_t skill_count;
    airy_mtx_t lock;
    int initialized;
};

int market_service_create(const market_config_t *config, market_service_t **service)
{
    market_config_t default_cfg;
    if (!service) {
        SVC_LOG_ERROR("market_service_create: NULL service output parameter");
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!config) {
        __builtin_memset(&default_cfg, 0, sizeof(default_cfg));
        default_cfg.cache_ttl_ms = 3600000;
        default_cfg.sync_interval_ms = 30000;
        config = &default_cfg;
    }

    market_service_t *svc = (market_service_t *)AIRY_CALLOC(1, sizeof(market_service_t));
    if (!svc) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate service struct");
    }

    __builtin_memcpy(&svc->config, config, sizeof(market_config_t));
    if (config->registry_url)
        svc->config.registry_url = AIRY_STRDUP(config->registry_url);
    if (config->storage_path)
        svc->config.storage_path = AIRY_STRDUP(config->storage_path);

    airy_mtx_init(&svc->lock);
    svc->initialized = 1;
    *service = svc;
    return 0;
}

int market_service_destroy(market_service_t *service)
{
    if (!service) {
        SVC_LOG_ERROR("market_service_destroy: NULL service parameter");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]) {
            AIRY_FREE(service->agents[i]->agent_id);
            AIRY_FREE(service->agents[i]->name);
            AIRY_FREE(service->agents[i]->version);
            AIRY_FREE(service->agents[i]->description);
            AIRY_FREE(service->agents[i]->author);
            AIRY_FREE(service->agents[i]->repository);
            AIRY_FREE(service->agents[i]->dependencies);
            AIRY_FREE(service->agents[i]);
        }
    }

    for (size_t i = 0; i < service->skill_count; i++) {
        if (service->skills[i]) {
            AIRY_FREE(service->skills[i]->skill_id);
            AIRY_FREE(service->skills[i]->name);
            AIRY_FREE(service->skills[i]->version);
            AIRY_FREE(service->skills[i]->description);
            AIRY_FREE(service->skills[i]->author);
            AIRY_FREE(service->skills[i]->repository);
            AIRY_FREE(service->skills[i]->dependencies);
            AIRY_FREE(service->skills[i]);
        }
    }

    AIRY_FREE((void *)service->config.registry_url);
    AIRY_FREE((void *)service->config.storage_path);
    airy_mtx_unlock(&service->lock);
    airy_mtx_destroy(&service->lock);
    AIRY_FREE(service);
    return 0;
}

int market_service_register_agent(market_service_t *service, const agent_info_t *agent_info)
{
    if (!service || !agent_info || !service->initialized) {
        SVC_LOG_ERROR("market_service_register_agent: NULL parameter or not initialized "
                      "(service=%p, agent_info=%p, initialized=%d)",
                      (const void *)service, (const void *)agent_info,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (service->agent_count >= AIRY_CAP_MAX_AGENTS) {
        SVC_LOG_ERROR("market_service_register_agent: max agents exceeded (count=%zu, max=%d)",
                      service->agent_count, AIRY_CAP_MAX_AGENTS);
        AIRY_ERROR(AIRY_ERR_OVERFLOW, "max agents exceeded");
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_info->agent_id) == 0) {
            AIRY_FREE(service->agents[i]->name);
            service->agents[i]->name = NULL;
            AIRY_FREE(service->agents[i]->version);
            service->agents[i]->version = NULL;
            AIRY_FREE(service->agents[i]->description);
            service->agents[i]->description = NULL;
            AIRY_FREE(service->agents[i]->author);
            service->agents[i]->author = NULL;
            AIRY_FREE(service->agents[i]->repository);
            service->agents[i]->repository = NULL;
            AIRY_FREE(service->agents[i]->dependencies);
            service->agents[i]->dependencies = NULL;

            service->agents[i]->name = agent_info->name ? AIRY_STRDUP(agent_info->name) : NULL;
            service->agents[i]->version =
                agent_info->version ? AIRY_STRDUP(agent_info->version) : NULL;
            service->agents[i]->description =
                agent_info->description ? AIRY_STRDUP(agent_info->description) : NULL;
            service->agents[i]->type = agent_info->type;
            service->agents[i]->status = agent_info->status;
            service->agents[i]->author =
                agent_info->author ? AIRY_STRDUP(agent_info->author) : NULL;
            service->agents[i]->repository =
                agent_info->repository ? AIRY_STRDUP(agent_info->repository) : NULL;
            service->agents[i]->dependencies =
                agent_info->dependencies ? AIRY_STRDUP(agent_info->dependencies) : NULL;
            service->agents[i]->rating = agent_info->rating;
            service->agents[i]->download_count = agent_info->download_count;
            service->agents[i]->last_updated = (uint64_t)time(NULL);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    agent_info_t *new_agent = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
    if (!new_agent) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("market_service_register_agent: calloc failed for new agent entry");
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate agent entry");
    }

    new_agent->agent_id = agent_info->agent_id ? AIRY_STRDUP(agent_info->agent_id) : NULL;
    new_agent->name = agent_info->name ? AIRY_STRDUP(agent_info->name) : NULL;
    new_agent->version = agent_info->version ? AIRY_STRDUP(agent_info->version) : NULL;
    new_agent->description = agent_info->description ? AIRY_STRDUP(agent_info->description) : NULL;
    new_agent->type = agent_info->type;
    new_agent->status = agent_info->status;
    new_agent->author = agent_info->author ? AIRY_STRDUP(agent_info->author) : NULL;
    new_agent->repository = agent_info->repository ? AIRY_STRDUP(agent_info->repository) : NULL;
    new_agent->dependencies =
        agent_info->dependencies ? AIRY_STRDUP(agent_info->dependencies) : NULL;
    if (!new_agent->agent_id || !new_agent->name || !new_agent->version) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("market_service_register_agent: strdup failed for required agent fields "
                      "(agent_id=%p, name=%p, version=%p)",
                      (const void *)new_agent->agent_id, (const void *)new_agent->name,
                      (const void *)new_agent->version);
        AIRY_FREE(new_agent->agent_id);
        AIRY_FREE(new_agent->name);
        AIRY_FREE(new_agent->version);
        AIRY_FREE(new_agent->description);
        AIRY_FREE(new_agent->author);
        AIRY_FREE(new_agent->repository);
        AIRY_FREE(new_agent->dependencies);
        AIRY_FREE(new_agent);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate agent required fields");
    }
    new_agent->rating = agent_info->rating;
    new_agent->download_count = agent_info->download_count;
    new_agent->last_updated = (uint64_t)time(NULL);

    service->agents[service->agent_count++] = new_agent;
    airy_mtx_unlock(&service->lock);
    return 0;
}

int market_service_register_skill(market_service_t *service, const skill_info_t *skill_info)
{
    if (!service || !skill_info || !service->initialized) {
        SVC_LOG_ERROR("market_service_register_skill: NULL parameter or not initialized "
                      "(service=%p, skill_info=%p, initialized=%d)",
                      (const void *)service, (const void *)skill_info,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (service->skill_count >= MAX_SKILLS) {
        SVC_LOG_ERROR("market_service_register_skill: max skills exceeded (count=%zu, max=%d)",
                      service->skill_count, MAX_SKILLS);
        AIRY_ERROR(AIRY_ERR_OVERFLOW, "max skills exceeded");
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->skill_count; i++) {
        if (strcmp(service->skills[i]->skill_id, skill_info->skill_id) == 0) {
            AIRY_FREE(service->skills[i]->name);
            service->skills[i]->name = NULL;
            AIRY_FREE(service->skills[i]->version);
            service->skills[i]->version = NULL;
            AIRY_FREE(service->skills[i]->description);
            service->skills[i]->description = NULL;
            AIRY_FREE(service->skills[i]->author);
            service->skills[i]->author = NULL;
            AIRY_FREE(service->skills[i]->repository);
            service->skills[i]->repository = NULL;
            AIRY_FREE(service->skills[i]->dependencies);
            service->skills[i]->dependencies = NULL;

            service->skills[i]->name = skill_info->name ? AIRY_STRDUP(skill_info->name) : NULL;
            service->skills[i]->version =
                skill_info->version ? AIRY_STRDUP(skill_info->version) : NULL;
            service->skills[i]->description =
                skill_info->description ? AIRY_STRDUP(skill_info->description) : NULL;
            service->skills[i]->type = skill_info->type;
            service->skills[i]->author =
                skill_info->author ? AIRY_STRDUP(skill_info->author) : NULL;
            service->skills[i]->repository =
                skill_info->repository ? AIRY_STRDUP(skill_info->repository) : NULL;
            service->skills[i]->dependencies =
                skill_info->dependencies ? AIRY_STRDUP(skill_info->dependencies) : NULL;
            service->skills[i]->rating = skill_info->rating;
            service->skills[i]->download_count = skill_info->download_count;
            service->skills[i]->last_updated = (uint64_t)time(NULL);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    skill_info_t *new_skill = (skill_info_t *)AIRY_CALLOC(1, sizeof(skill_info_t));
    if (!new_skill) {
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate skill entry");
    }

    new_skill->skill_id = skill_info->skill_id ? AIRY_STRDUP(skill_info->skill_id) : NULL;
    new_skill->name = skill_info->name ? AIRY_STRDUP(skill_info->name) : NULL;
    new_skill->version = skill_info->version ? AIRY_STRDUP(skill_info->version) : NULL;
    new_skill->description = skill_info->description ? AIRY_STRDUP(skill_info->description) : NULL;
    new_skill->type = skill_info->type;
    new_skill->author = skill_info->author ? AIRY_STRDUP(skill_info->author) : NULL;
    new_skill->repository = skill_info->repository ? AIRY_STRDUP(skill_info->repository) : NULL;
    new_skill->dependencies =
        skill_info->dependencies ? AIRY_STRDUP(skill_info->dependencies) : NULL;
    if (!new_skill->skill_id || !new_skill->name || !new_skill->version) {
        AIRY_FREE(new_skill->skill_id);
        AIRY_FREE(new_skill->name);
        AIRY_FREE(new_skill->version);
        AIRY_FREE(new_skill->description);
        AIRY_FREE(new_skill->author);
        AIRY_FREE(new_skill->repository);
        AIRY_FREE(new_skill->dependencies);
        AIRY_FREE(new_skill);
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate skill required fields");
    }
    new_skill->rating = skill_info->rating;
    new_skill->download_count = skill_info->download_count;
    new_skill->last_updated = (uint64_t)time(NULL);

    service->skills[service->skill_count++] = new_skill;
    airy_mtx_unlock(&service->lock);
    return 0;
}

int market_service_search_agents(market_service_t *service, const search_params_t *params,
                                 agent_info_t ***agents, size_t *count)
{
    if (!service || !params || !agents || !count || !service->initialized) {
        SVC_LOG_ERROR("market_service_search_agents: NULL parameter or not initialized "
                      "(service=%p, params=%p, agents=%p, count=%p, initialized=%d)",
                      (const void *)service, (const void *)params, (const void *)agents,
                      (const void *)count, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    size_t results_size = 16;
    agent_info_t **results = (agent_info_t **)AIRY_MALLOC(sizeof(agent_info_t *) * results_size);
    if (!results) {
        SVC_LOG_ERROR("market_service_search_agents: malloc failed for search results");
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate search results");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (params->query && strlen(params->query) > 0) {
            if (!strstr(service->agents[i]->agent_id, params->query) &&
                !strstr(service->agents[i]->name, params->query) &&
                !(service->agents[i]->description &&
                  strstr(service->agents[i]->description, params->query))) {
                continue;
            }
        }

        if (found >= results_size) {
            /* 倍增溢出检查：results_size * 2 * sizeof(agent_info_t *) 不得回绕，
             * 溢出时停止扩容，返回已收集的部分结果 */
            if (results_size > SIZE_MAX / (2 * sizeof(agent_info_t *)))
                break;
            results_size *= 2;
            agent_info_t **tmp =
                (agent_info_t **)AIRY_REALLOC(results, sizeof(agent_info_t *) * results_size);
            if (!tmp) {
                SVC_LOG_ERROR("market_service_search_agents: realloc failed for search results "
                              "(results_size=%zu)",
                              results_size);
                AIRY_FREE(results);
                airy_mtx_unlock(&service->lock);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize search results");
            }
            results = tmp;
        }

        results[found++] = service->agents[i];
        if (params->limit > 0 && found >= params->limit)
            break;
    }
    airy_mtx_unlock(&service->lock);

    *agents = results;
    *count = found;
    return 0;
}

int market_service_search_skills(market_service_t *service, const search_params_t *params,
                                 skill_info_t ***skills, size_t *count)
{
    if (!service || !params || !skills || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    size_t results_size = 16;
    skill_info_t **results = (skill_info_t **)AIRY_MALLOC(sizeof(skill_info_t *) * results_size);
    if (!results) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate skill search results");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->skill_count; i++) {
        if (params->query && strlen(params->query) > 0) {
            if (!strstr(service->skills[i]->skill_id, params->query) &&
                !strstr(service->skills[i]->name, params->query) &&
                !(service->skills[i]->description &&
                  strstr(service->skills[i]->description, params->query))) {
                continue;
            }
        }

        if (found >= results_size) {
            /* 倍增溢出检查：results_size * 2 * sizeof(skill_info_t *) 不得回绕，
             * 溢出时停止扩容，返回已收集的部分结果 */
            if (results_size > SIZE_MAX / (2 * sizeof(skill_info_t *)))
                break;
            results_size *= 2;
            skill_info_t **tmp =
                (skill_info_t **)AIRY_REALLOC(results, sizeof(skill_info_t *) * results_size);
            if (!tmp) {
                AIRY_FREE(results);
                airy_mtx_unlock(&service->lock);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize skill search results");
            }
            results = tmp;
        }

        results[found++] = service->skills[i];
        if (params->limit > 0 && found >= params->limit)
            break;
    }
    airy_mtx_unlock(&service->lock);

    *skills = results;
    *count = found;
    return 0;
}

int market_service_install_agent(market_service_t *service, const install_request_t *request,
                                 install_result_t **result)
{
    if (!service || !request || !result || !service->initialized) {
        SVC_LOG_ERROR("market_service_install_agent: NULL parameter or not initialized "
                      "(service=%p, request=%p, result=%p, initialized=%d)",
                      (const void *)service, (const void *)request, (const void *)result,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!is_safe_path_component(request->id)) {
        SVC_LOG_ERROR(
            "market_service_install_agent: unsafe path component in install request id (id=%s)",
            request->id ? request->id : "NULL");
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "install request id is unsafe");
    }

    install_result_t *res = (install_result_t *)AIRY_CALLOC(1, sizeof(install_result_t));
    if (!res) {
        SVC_LOG_ERROR("market_service_install_agent: calloc failed for install result");
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate install result");
    }

    airy_mtx_lock(&service->lock);
    size_t target_idx = (size_t)-1;
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, request->id) == 0) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == (size_t)-1) {
        airy_mtx_unlock(&service->lock);
        res->success = false;
        res->message = AIRY_STRDUP("Agent not found");
        res->error_code = -3;
        *result = res;
        return 0;
    }

    agent_info_t *target = service->agents[target_idx];
    char *snap_agent_id = target->agent_id ? AIRY_STRDUP(target->agent_id) : NULL;
    char *snap_name = target->name ? AIRY_STRDUP(target->name) : NULL;
    char *snap_version = target->version ? AIRY_STRDUP(target->version) : NULL;
    char *snap_author = target->author ? AIRY_STRDUP(target->author) : NULL;
    char *snap_repo = target->repository ? AIRY_STRDUP(target->repository) : NULL;
    char *snap_storage =
        service->config.storage_path ? AIRY_STRDUP(service->config.storage_path) : NULL;
    airy_mtx_unlock(&service->lock);

    if (!snap_agent_id) {
        AIRY_FREE(snap_name);
        AIRY_FREE(snap_version);
        AIRY_FREE(snap_author);
        AIRY_FREE(snap_repo);
        AIRY_FREE(snap_storage);
        AIRY_FREE(res);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *base_path = request->install_path ?
                                request->install_path :
                                (snap_storage ? snap_storage : market_default_storage("agents"));
    char install_dir[1024];
    snprintf(install_dir, sizeof(install_dir), "%s/%s", base_path, request->id);

    {
        char _par[1024];
        snprintf(_par, sizeof(_par), "%s", base_path);
        size_t _plen = strlen(_par);
        while (_plen > 1 && _par[_plen - 1] == '/')
            _par[--_plen] = '\0';
        for (char *p = _par + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                int _m = mkdir(_par, 0755);
                *p = '/';
                if (_m != 0 && errno != EEXIST) {
                    SVC_LOG_ERROR(
                        "market_service_install_agent: mkdir failed for parent (path=%s, errno=%d)",
                        _par, errno);
                    break;
                }
            }
        }
        if (mkdir(_par, 0755) != 0 && errno != EEXIST) {
            SVC_LOG_ERROR(
                "market_service_install_agent: mkdir failed for parent root (path=%s, errno=%d)",
                _par, errno);
        }
    }

    {
        int mkret = mkdir(install_dir, 0755);
        if (mkret != 0 && errno != EEXIST) {
            SVC_LOG_ERROR("market_service_install_agent: mkdir failed for install directory "
                          "(path=%s, errno=%d)",
                          install_dir, errno);
            res->success = false;
            res->message = AIRY_STRDUP("Failed to create install directory");
            res->error_code = -4;
            *result = res;
            AIRY_FREE(snap_agent_id);
            AIRY_FREE(snap_name);
            AIRY_FREE(snap_version);
            AIRY_FREE(snap_author);
            AIRY_FREE(snap_repo);
            AIRY_FREE(snap_storage);
            return 0;
        }
    }

    char meta_path[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(meta_path, sizeof(meta_path), "%s/agent.json", install_dir);
#pragma GCC diagnostic pop
    FILE *meta_fp = fopen(meta_path, "w");
    if (meta_fp) {
        char _mi_buf[1024];
        fputs("{\n", meta_fp);
        snprintf(_mi_buf, sizeof(_mi_buf), "  \"agent_id\": \"%s\",\n",
                 snap_agent_id ? snap_agent_id : "");
        fputs(_mi_buf, meta_fp);
        snprintf(_mi_buf, sizeof(_mi_buf), "  \"name\": \"%s\",\n", snap_name ? snap_name : "");
        fputs(_mi_buf, meta_fp);
        snprintf(_mi_buf, sizeof(_mi_buf), "  \"version\": \"%s\",\n",
                 request->version ? request->version : (snap_version ? snap_version : "0.0.1"));
        fputs(_mi_buf, meta_fp);
        snprintf(_mi_buf, sizeof(_mi_buf), "  \"author\": \"%s\",\n",
                 snap_author ? snap_author : "");
        fputs(_mi_buf, meta_fp);
        fputs("  \"status\": \"installed\",\n", meta_fp);
        snprintf(_mi_buf, sizeof(_mi_buf), "  \"installed_at\": %lld\n", (long long)time(NULL));
        fputs(_mi_buf, meta_fp);
        fputs("}\n", meta_fp);
        fclose(meta_fp);
    }

    if (snap_repo && strlen(snap_repo) > 0 && is_valid_url(snap_repo)) {
        char download_path[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(download_path, sizeof(download_path), "%s/package.tar.gz", install_dir);
#pragma GCC diagnostic pop

#ifdef _WIN32
        /* Windows 等价：用 win_run_command（CreateProcess）替代 fork/execlp/waitpid。
         * 行为对齐：curl 失败则仅元数据安装；成功则解压 tar 并清理下载文件。 */
        {
            const char *const curl_args[] = {"-sfL", "-o", download_path, snap_repo, NULL};
            int curl_ret = win_run_command("curl", curl_args);
            if (curl_ret != 0) {
                SVC_LOG_WARN(
                    "Download failed for agent %s from %s (curl_ret=%d), metadata only install",
                    request->id, snap_repo, curl_ret);
            } else {
                const char *const tar_args[] = {"-xzf", download_path, "-C", install_dir, NULL};
                win_run_command("tar", tar_args);
                remove(download_path);
            }
        }
#else
        pid_t curl_pid = fork();
        if (curl_pid == 0) {
            execlp("curl", "curl", "-sfL", "-o", download_path, snap_repo, (char *)NULL);
            _exit(127);
        } else if (curl_pid > 0) {
            int curl_status = 0;
            waitpid(curl_pid, &curl_status, 0);
            int curl_ret = WIFEXITED(curl_status) ? WEXITSTATUS(curl_status) : -1;
            if (curl_ret != 0) {
                SVC_LOG_WARN(
                    "Download failed for agent %s from %s (curl_ret=%d), metadata only install",
                    request->id, snap_repo, curl_ret);
            } else {
                pid_t tar_pid = fork();
                if (tar_pid == 0) {
                    execlp("tar", "tar", "-xzf", download_path, "-C", install_dir, (char *)NULL);
                    _exit(127);
                } else if (tar_pid > 0) {
                    int tar_status = 0;
                    waitpid(tar_pid, &tar_status, 0);
                }
                remove(download_path);
            }
        } else {
            SVC_LOG_WARN("fork failed for agent %s download: %s", request->id, strerror(errno));
        }
#endif
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, request->id) == 0) {
            service->agents[i]->status = AGENT_STATUS_AVAILABLE;
            service->agents[i]->download_count++;
            break;
        }
    }
    airy_mtx_unlock(&service->lock);

    res->success = true;
    res->message = AIRY_STRDUP("Agent installed successfully");
    res->installed_version =
        AIRY_STRDUP(request->version ? request->version : (snap_version ? snap_version : "0.0.1"));
    res->install_path = AIRY_STRDUP(install_dir);
    res->error_code = 0;

    AIRY_FREE(snap_agent_id);
    AIRY_FREE(snap_name);
    AIRY_FREE(snap_version);
    AIRY_FREE(snap_author);
    AIRY_FREE(snap_repo);
    AIRY_FREE(snap_storage);

    *result = res;
    return 0;
}

int market_service_install_skill(market_service_t *service, const install_request_t *request,
                                 install_result_t **result)
{
    if (!service || !request || !result || !service->initialized) {
        SVC_LOG_ERROR("market_service_install_skill: NULL parameter or not initialized "
                      "(service=%p, request=%p, result=%p, initialized=%d)",
                      (const void *)service, (const void *)request, (const void *)result,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    install_result_t *res = (install_result_t *)AIRY_CALLOC(1, sizeof(install_result_t));
    if (!res) {
        SVC_LOG_ERROR("market_service_install_skill: calloc failed for skill install result");
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate skill install result");
    }

    airy_mtx_lock(&service->lock);
    skill_info_t *target = NULL;
    for (size_t i = 0; i < service->skill_count; i++) {
        if (strcmp(service->skills[i]->skill_id, request->id) == 0) {
            target = service->skills[i];
            break;
        }
    }

    if (!target) {
        airy_mtx_unlock(&service->lock);
        res->success = false;
        res->message = AIRY_STRDUP("Skill not found");
        res->error_code = -3;
        *result = res;
        return 0;
    }

    target->download_count++;

    res->success = true;
    res->message = AIRY_STRDUP("Skill installed successfully");
    res->installed_version = AIRY_STRDUP(request->version ? request->version : target->version);
    res->install_path = AIRY_STRDUP(request->install_path ? request->install_path :
                                                            market_default_storage("skills"));
    res->error_code = 0;
    airy_mtx_unlock(&service->lock);

    *result = res;
    return 0;
}

int market_service_uninstall_agent(market_service_t *service, const char *agent_id)
{
    if (!service || !agent_id || !service->initialized) {
        SVC_LOG_ERROR("market_service_uninstall_agent: NULL parameter or not initialized "
                      "(service=%p, agent_id=%p, initialized=%d)",
                      (const void *)service, (const void *)agent_id,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!is_safe_path_component(agent_id)) {
        SVC_LOG_ERROR(
            "market_service_uninstall_agent: unsafe path component in agent_id (agent_id=%s)",
            agent_id);
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "agent_id is unsafe path component");
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_id) == 0) {
            const char *storage = service->config.storage_path ? service->config.storage_path :
                                                                 market_default_storage("agents");
            char install_dir[1024];
            snprintf(install_dir, sizeof(install_dir), "%s/%s", storage, agent_id);

            int rm_ret = recursive_remove(install_dir);
            if (rm_ret != 0) {
                SVC_LOG_WARN("Failed to remove install directory: %s (ret=%d)", install_dir,
                             rm_ret);
            }

            service->agents[i]->status = AGENT_STATUS_DISABLED;
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&service->lock);
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "agent not found for uninstall");
}

int market_service_uninstall_skill(market_service_t *service, const char *skill_id)
{
    if (!service || !skill_id || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->skill_count; i++) {
        if (strcmp(service->skills[i]->skill_id, skill_id) == 0) {
            AIRY_FREE(service->skills[i]->skill_id);
            AIRY_FREE(service->skills[i]->name);
            AIRY_FREE(service->skills[i]->version);
            AIRY_FREE(service->skills[i]->description);
            AIRY_FREE(service->skills[i]->author);
            AIRY_FREE(service->skills[i]->repository);
            AIRY_FREE(service->skills[i]->dependencies);
            AIRY_FREE(service->skills[i]);

            for (size_t j = i; j < service->skill_count - 1; j++) {
                service->skills[j] = service->skills[j + 1];
            }
            service->skill_count--;
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&service->lock);
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "skill not found for uninstall");
}

int market_service_get_installed_agents(market_service_t *service, agent_info_t ***agents,
                                        size_t *count)
{
    if (!service || !agents || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    size_t results_size = 16;
    agent_info_t **results = (agent_info_t **)AIRY_MALLOC(sizeof(agent_info_t *) * results_size);
    if (!results) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate installed agents list");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]->status == AGENT_STATUS_AVAILABLE ||
            service->agents[i]->status == AGENT_STATUS_ERROR) {

            if (found >= results_size) {
                /* 倍增溢出检查：results_size * 2 * sizeof(agent_info_t *) 不得回绕，
                 * 溢出时停止扩容，返回已收集的部分结果 */
                if (results_size > SIZE_MAX / (2 * sizeof(agent_info_t *)))
                    break;
                results_size *= 2;
                agent_info_t **tmp =
                    (agent_info_t **)AIRY_REALLOC(results, sizeof(agent_info_t *) * results_size);
                if (!tmp) {
                    AIRY_FREE(results);
                    airy_mtx_unlock(&service->lock);
                    AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize installed agents list");
                }
                results = tmp;
            }

            results[found++] = service->agents[i];
        }
    }
    airy_mtx_unlock(&service->lock);

    *agents = results;
    *count = found;
    return 0;
}

int market_service_get_installed_skills(market_service_t *service, skill_info_t ***skills,
                                        size_t *count)
{
    if (!service || !skills || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    size_t results_size = 16;
    skill_info_t **results = (skill_info_t **)AIRY_MALLOC(sizeof(skill_info_t *) * results_size);
    if (!results) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate installed skills list");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->skill_count; i++) {
        if (found >= results_size) {
            /* 倍增溢出检查：results_size * 2 * sizeof(skill_info_t *) 不得回绕，
             * 溢出时停止扩容，返回已收集的部分结果 */
            if (results_size > SIZE_MAX / (2 * sizeof(skill_info_t *)))
                break;
            results_size *= 2;
            skill_info_t **tmp =
                (skill_info_t **)AIRY_REALLOC(results, sizeof(skill_info_t *) * results_size);
            if (!tmp) {
                AIRY_FREE(results);
                airy_mtx_unlock(&service->lock);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize installed skills list");
            }
            results = tmp;
        }

        results[found++] = service->skills[i];
    }
    airy_mtx_unlock(&service->lock);

    *skills = results;
    *count = found;
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

int market_service_reload_config(market_service_t *service, const market_config_t *config)
{
    if (!service || !config || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&service->lock);

    // Save old owned pointers
    char *old_url = (char *)service->config.registry_url;
    char *old_path = (char *)service->config.storage_path;

    // Copy non-pointer fields, preserving old owned pointers temporarily
    {
        market_config_t tmp = *config;
        tmp.registry_url = old_url;
        tmp.storage_path = old_path;
        service->config = tmp;
    }

    // Replace pointer fields with our own copies
    char *new_url = config->registry_url ? AIRY_STRDUP(config->registry_url) : NULL;
    char *new_path = config->storage_path ? AIRY_STRDUP(config->storage_path) : NULL;

    if (config->registry_url && !new_url) {
        AIRY_FREE(new_path);
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate registry_url");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    if (config->storage_path && !new_path) {
        AIRY_FREE(new_url);
        service->config.registry_url = new_url;
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate storage_path");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    service->config.registry_url = new_url;
    service->config.storage_path = new_path;
    AIRY_FREE(old_url);
    AIRY_FREE(old_path);
    airy_mtx_unlock(&service->lock);

    return 0;
}

int market_service_sync_registry(market_service_t *service)
{
    if (!service || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&service->lock);
    if (!service->config.enable_remote_registry) {
        airy_mtx_unlock(&service->lock);
        return 0;
    }

    if (!service->config.registry_url || strlen(service->config.registry_url) == 0) {
        SVC_LOG_WARN("Sync registry: no registry_url configured");
        airy_mtx_unlock(&service->lock);
        return 0;
    }

    if (!is_safe_for_shell(service->config.registry_url)) {
        SVC_LOG_WARN("Sync registry: invalid registry_url, possible injection detected");
        airy_mtx_unlock(&service->lock);
        return 0;
    }

    char *snap_url = AIRY_STRDUP(service->config.registry_url);
    char *snap_storage =
        service->config.storage_path ? AIRY_STRDUP(service->config.storage_path) : NULL;
    airy_mtx_unlock(&service->lock);

    if (!snap_url) {
        AIRY_FREE(snap_storage);
        SVC_LOG_ERROR("Sync registry: strdup failed for registry_url");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *storage = snap_storage ? snap_storage : AIRY_CACHE_DIR;

    {
        size_t pos = 0;
        char tmp[1024];
        AIRY_STRNCPY_TERM(tmp, storage, sizeof(tmp));
        (tmp)[sizeof(tmp) - 1] = '\0';
        tmp[sizeof(tmp) - 1] = '\0';
        while (tmp[pos]) {
            if (tmp[pos] == '/' && pos > 0) {
                tmp[pos] = '\0';
                mkdir(tmp, 0755);
                tmp[pos] = '/';
            }
            pos++;
        }
        mkdir(tmp, 0755);
    }

    char index_path[1024];
    snprintf(index_path, sizeof(index_path), "%s/registry_index.json", storage);

    char url[2048];
    if (strncmp(snap_url, "http", 4) == 0) {
        snprintf(url, sizeof(url), "%s/index.json", snap_url);
    } else {
        snprintf(url, sizeof(url), "https://%s/index.json", snap_url);
    }

#ifdef _WIN32
    /* Windows 等价：用 win_run_command（CreateProcess）替代 fork/execlp/waitpid。
     * 行为对齐：curl 失败（含 CreateProcess 找不到 curl）则告警并返回 0。 */
    {
        const char *const curl_args[] = {"-sfL", "-o",         index_path, url, "--connect-timeout",
                                         "10",   "--max-time", "60",       NULL};
        int curl_ret = win_run_command("curl", curl_args);
        if (curl_ret != 0) {
            SVC_LOG_WARN("Sync registry: download failed from %s (curl_ret=%d)", url, curl_ret);
            AIRY_FREE(snap_url);
            AIRY_FREE(snap_storage);
            return 0;
        }
    }
#else
    pid_t curl_pid = fork();
    if (curl_pid == 0) {
        execlp("curl", "curl", "-sfL", "-o", index_path, url, "--connect-timeout", "10",
               "--max-time", "60", (char *)NULL);
        _exit(127);
    } else if (curl_pid > 0) {
        int curl_status = 0;
        waitpid(curl_pid, &curl_status, 0);
        int curl_ret = WIFEXITED(curl_status) ? WEXITSTATUS(curl_status) : -1;
        if (curl_ret != 0) {
            SVC_LOG_WARN("Sync registry: download failed from %s (curl_ret=%d)", url, curl_ret);
            AIRY_FREE(snap_url);
            AIRY_FREE(snap_storage);
            return 0;
        }
    } else {
        SVC_LOG_WARN("Sync registry: fork failed: %s", strerror(errno));
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        AIRY_ERROR(AIRY_ERR_IO, "fork failed during sync");
    }
#endif

    FILE *idx_fp = fopen(index_path, "r");
    if (!idx_fp) {
        SVC_LOG_WARN("Sync registry: cannot open downloaded index %s", index_path);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        return 0;
    }

    fseek(idx_fp, 0, SEEK_END);
    long fsize = ftell(idx_fp);
    fseek(idx_fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
        fclose(idx_fp);
        SVC_LOG_WARN("Sync registry: invalid index size %ld", fsize);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        return 0;
    }

    char *idx_data = (char *)AIRY_MALLOC((size_t)fsize + 1);
    if (!idx_data) {
        fclose(idx_fp);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate index buffer");
    }
    size_t nread = fread(idx_data, 1, (size_t)fsize, idx_fp);
    if (nread != (size_t)fsize) {
        AIRY_FREE(idx_data);
        idx_data = NULL;
        fclose(idx_fp);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        AIRY_ERROR(AIRY_ERR_IO, "fread index file failed");
    }
    idx_data[nread] = '\0';
    fclose(idx_fp);

    char found_ids[256][128];
    int n_found = 0;
    char *entry = strstr(idx_data, "\"agent_id\"");
    while (entry && n_found < 256) {
        char *id_start = strchr(entry, ':');
        if (!id_start)
            break;
        id_start++;
        while (*id_start && (*id_start == ' ' || *id_start == '\t' || *id_start == '"'))
            id_start++;
        char *id_end = id_start;
        while (*id_end && *id_end != '"' && *id_end != ',' && *id_end != '}')
            id_end++;

        size_t id_len = (size_t)(id_end - id_start);
        if (id_len > 0 && id_len < 128) {
            __builtin_memcpy(found_ids[n_found], id_start, id_len);
            found_ids[n_found][id_len] = '\0';
            n_found++;
        }

        entry = strstr(id_end + 1, "\"agent_id\"");
    }
    AIRY_FREE(idx_data);
    idx_data = NULL;

    int synced = 0;
    airy_mtx_lock(&service->lock);
    for (int k = 0; k < n_found; k++) {
        int already_exists = 0;
        for (size_t i = 0; i < service->agent_count; i++) {
            if (strcmp(service->agents[i]->agent_id, found_ids[k]) == 0) {
                already_exists = 1;
                break;
            }
        }

        if (!already_exists && service->agent_count < AIRY_CAP_MAX_AGENTS) {
            agent_info_t *new_agent = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
            if (new_agent) {
                new_agent->agent_id = AIRY_STRDUP(found_ids[k]);
                new_agent->name = AIRY_STRDUP(found_ids[k]);
                new_agent->version = AIRY_STRDUP("latest");
                new_agent->status = AGENT_STATUS_AVAILABLE;
                service->agents[service->agent_count++] = new_agent;
                synced++;
            }
        }
    }
    airy_mtx_unlock(&service->lock);

    AIRY_FREE(snap_url);
    AIRY_FREE(snap_storage);
    SVC_LOG_INFO("Sync registry: synced %d new agents from %s", synced, url);
    return 0;
}
