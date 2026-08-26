// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_install.c
 * @brief Market service install domain: agent/skill install (including
 *        download and unpack) and uninstall (including directory cleanup).
 */

#include "airy_memory.h"
#include "error.h"
#include "io.h"
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
#include <unistd.h>
#include <sys/wait.h>
#else
#include <windows.h>
#endif

#include "market_service_internal.h"

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
    /* P1-6：安装元数据原子写（同目录 tmp + fsync + rename）。半写的
     * agent.json 会使已安装状态不可识别，且失败不得静默（原实现
     * fopen 失败仅跳过，后续仍返回 success）。 */
    char meta_json[2048];
    int mlen = snprintf(meta_json, sizeof(meta_json),
                        "{\n  \"agent_id\": \"%s\",\n  \"name\": \"%s\",\n"
                        "  \"version\": \"%s\",\n  \"author\": \"%s\",\n"
                        "  \"status\": \"installed\",\n  \"installed_at\": %lld\n}\n",
                        snap_agent_id ? snap_agent_id : "", snap_name ? snap_name : "",
                        request->version ? request->version : (snap_version ? snap_version : "0.0.1"),
                        snap_author ? snap_author : "", (long long)time(NULL));
    if (mlen < 0 || mlen >= (int)sizeof(meta_json) ||
        airy_io_write_file(meta_path, meta_json, (size_t)mlen) != 0) {
        res->success = false;
        res->message = AIRY_STRDUP("Failed to write install metadata");
        res->error_code = -5;
        *result = res;
        AIRY_FREE(snap_agent_id);
        AIRY_FREE(snap_name);
        AIRY_FREE(snap_version);
        AIRY_FREE(snap_author);
        AIRY_FREE(snap_repo);
        AIRY_FREE(snap_storage);
        return 0;
    }

    if (snap_repo && strlen(snap_repo) > 0 && is_valid_url(snap_repo)) {
        char download_path[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(download_path, sizeof(download_path), "%s/package.tar.gz", install_dir);
#pragma GCC diagnostic pop

#ifdef _WIN32
        /* Windows equivalent: win_run_command (CreateProcess) replaces
         * fork/execlp/waitpid. Behavior aligned: on curl failure only install
         * metadata; on success extract the tar and clean up the download. */
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
