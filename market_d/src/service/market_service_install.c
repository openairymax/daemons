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
#include "platform_misc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#else
#include <windows.h>
#endif

#include "market_service_internal.h"

/* 安装装配上下文：agent 字段在持锁期间拷出，解锁后原对象可能被并发改写
 * 或释放。mkt_ 为本文件装配阶段函数的统一前缀（G11 名字长度约束）。 */
typedef struct {
    market_service_t *svc;
    const install_request_t *req;
    install_result_t *res;
    install_result_t **out;
    char *id;
    char *name;
    char *version;
    char *author;
    char *repo;
    char *storage;
    const char *ver;
    char dir[1024];
} mkt_inst_t;

static size_t mkt_locate(const market_service_t *svc, const char *id)
{
    for (size_t i = 0; i < svc->agent_count; i++) {
        if (strcmp(svc->agents[i]->agent_id, id) == 0)
            return i;
    }
    return (size_t)-1;
}

/* 安装入口公共前置校验：两个 install 入口同一套参数与初始化判定。 */
static int mkt_ready(const market_service_t *svc, const install_request_t *req,
                     install_result_t **out, const char *fn)
{
    if (svc && req && out && svc->initialized)
        return 0;
    SVC_LOG_ERROR("%s: NULL parameter or not initialized "
                  "(service=%p, request=%p, result=%p, initialized=%d)",
                  fn, (const void *)svc, (const void *)req, (const void *)out,
                  svc ? svc->initialized : -1);
    return AIRY_ERR_INVALID_PARAM;
}

/* 安装结果分配；失败仅记日志，错误码由调用点以自己的文案上报。 */
static install_result_t *mkt_res_new(const char *fn, const char *what)
{
    install_result_t *res = (install_result_t *)AIRY_CALLOC(1, sizeof(*res));
    if (!res)
        SVC_LOG_ERROR("%s: calloc failed for %s", fn, what);
    return res;
}

static void mkt_snap_free(mkt_inst_t *c)
{
    AIRY_FREE(c->id);
    AIRY_FREE(c->name);
    AIRY_FREE(c->version);
    AIRY_FREE(c->author);
    AIRY_FREE(c->repo);
    AIRY_FREE(c->storage);
}

/* 持锁拷出目标 agent 与存储路径：0=成功，1=未找到，-1=内存不足。 */
static int mkt_snap_take(mkt_inst_t *c)
{
    airy_mtx_lock(&c->svc->lock);
    size_t idx = mkt_locate(c->svc, c->req->id);
    if (idx == (size_t)-1) {
        airy_mtx_unlock(&c->svc->lock);
        return 1;
    }

    agent_info_t *target = c->svc->agents[idx];
    c->id = target->agent_id ? AIRY_STRDUP(target->agent_id) : NULL;
    c->name = target->name ? AIRY_STRDUP(target->name) : NULL;
    c->version = target->version ? AIRY_STRDUP(target->version) : NULL;
    c->author = target->author ? AIRY_STRDUP(target->author) : NULL;
    c->repo = target->repository ? AIRY_STRDUP(target->repository) : NULL;
    c->storage = c->svc->config.storage_path ? AIRY_STRDUP(c->svc->config.storage_path) : NULL;
    airy_mtx_unlock(&c->svc->lock);
    return c->id ? 0 : -1;
}

/* 选定目标目录并逐级创建：父目录失败仅记日志（既有语义），目标目录失败则中止。 */
static int mkt_dir_prep(mkt_inst_t *c)
{
    const char *base = c->req->install_path ?
                           c->req->install_path :
                           (c->storage ? c->storage : market_default_storage("agents"));
    snprintf(c->dir, sizeof(c->dir), "%s/%s", base, c->req->id);

    if (airy_mkdir_p(base) != 0)
        SVC_LOG_ERROR("market_service_install_agent: mkdir failed for parent (path=%s, errno=%d)",
                      base, errno);

    int ret = airy_io_ensure_dir(c->dir);
    if (ret != 0) {
        SVC_LOG_ERROR("market_service_install_agent: mkdir failed for install directory "
                      "(path=%s, errno=%d)",
                      c->dir, errno);
        return -1;
    }
    return 0;
}

/* P1-6：安装元数据原子写（同目录 tmp + fsync + rename）。半写的
 * agent.json 会使已安装状态不可识别，且失败不得静默。 */
static int mkt_meta_write(mkt_inst_t *c)
{
    /* 容量由 c->dir 推导，编译期可证不截断（无需 -Wformat-truncation 抑制）。 */
    char path[sizeof(c->dir) + 32];
    snprintf(path, sizeof(path), "%s/agent.json", c->dir);

    char json[2048];
    int len = snprintf(json, sizeof(json),
                       "{\n  \"agent_id\": \"%s\",\n  \"name\": \"%s\",\n"
                       "  \"version\": \"%s\",\n  \"author\": \"%s\",\n"
                       "  \"status\": \"installed\",\n  \"installed_at\": %lld\n}\n",
                       c->id ? c->id : "", c->name ? c->name : "", c->ver,
                       c->author ? c->author : "", (long long)time(NULL));
    if (len < 0 || len >= (int)sizeof(json) ||
        airy_io_write_file(path, json, (size_t)len) != 0)
        return -1;
    return 0;
}

#ifdef _WIN32
/* Windows 等价实现：win_run_command（CreateProcess）替代 fork/execlp/waitpid。
 * 行为对齐：curl 失败仅保留元数据；成功则解包并清理下载文件。 */
static void mkt_dl_win(mkt_inst_t *c, const char *dl)
{
    const char *const curl_args[] = {"-sfL", "-o", dl, c->repo, NULL};
    if (win_run_command("curl", curl_args) != 0) {
        SVC_LOG_WARN("Download failed for agent %s from %s (curl_ret=%d), metadata only install",
                     c->req->id, c->repo, 1);
        return;
    }
    const char *const tar_args[] = {"-xzf", dl, "-C", c->dir, NULL};
    win_run_command("tar", tar_args);
    remove(dl);
}
#else
/* 执行外部命令并等待退出：返回退出码；-1 表示 fork 失败或非正常终止。 */
static int mkt_run(const char *cmd, const char *a1, const char *a2, const char *a3,
                   const char *a4)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execlp(cmd, cmd, a1, a2, a3, a4, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void mkt_dl_posix(mkt_inst_t *c, const char *dl)
{
    int ret = mkt_run("curl", "-sfL", "-o", dl, c->repo);
    if (ret < 0) {
        SVC_LOG_WARN("fork failed for agent %s download: %s", c->req->id, strerror(errno));
        return;
    }
    if (ret > 0) {
        SVC_LOG_WARN("Download failed for agent %s from %s (curl_ret=%d), metadata only install",
                     c->req->id, c->repo, ret);
        return;
    }
    mkt_run("tar", "-xzf", dl, "-C", c->dir);
    remove(dl);
}
#endif

static void mkt_fetch_pkg(mkt_inst_t *c)
{
    if (!c->repo || !c->repo[0] || !is_valid_url(c->repo))
        return;

    char dl[sizeof(c->dir) + 32];
    snprintf(dl, sizeof(dl), "%s/package.tar.gz", c->dir);

#ifdef _WIN32
    mkt_dl_win(c, dl);
#else
    mkt_dl_posix(c, dl);
#endif
}

static void mkt_mark_done(market_service_t *svc, const char *id)
{
    airy_mtx_lock(&svc->lock);
    size_t idx = mkt_locate(svc, id);
    if (idx != (size_t)-1) {
        svc->agents[idx]->status = AGENT_STATUS_AVAILABLE;
        svc->agents[idx]->download_count++;
    }
    airy_mtx_unlock(&svc->lock);
}

/* 统一失败出口：结果落地并释放快照；语义为「已受理但未成功」，返回 0。 */
static int mkt_inst_fail(mkt_inst_t *c, int code, const char *msg)
{
    c->res->success = false;
    c->res->message = AIRY_STRDUP(msg);
    c->res->error_code = code;
    *c->out = c->res;
    mkt_snap_free(c);
    return 0;
}

int market_service_install_agent(market_service_t *service, const install_request_t *request,
                                 install_result_t **result)
{
    int rc = mkt_ready(service, request, result, "market_service_install_agent");
    if (rc != 0)
        return rc;
    if (!is_safe_path_component(request->id)) {
        SVC_LOG_ERROR(
            "market_service_install_agent: unsafe path component in install request id (id=%s)",
            request->id ? request->id : "NULL");
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "install request id is unsafe");
    }

    mkt_inst_t c = {0};
    c.svc = service;
    c.req = request;
    c.out = result;
    c.res = mkt_res_new("market_service_install_agent", "install result");
    if (!c.res)
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate install result");

    rc = mkt_snap_take(&c);
    if (rc > 0)
        return mkt_inst_fail(&c, -3, "Agent not found");
    if (rc < 0) {
        mkt_snap_free(&c);
        AIRY_FREE(c.res);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    c.ver = request->version ? request->version : (c.version ? c.version : "0.0.1");
    if (mkt_dir_prep(&c) != 0)
        return mkt_inst_fail(&c, -4, "Failed to create install directory");
    if (mkt_meta_write(&c) != 0)
        return mkt_inst_fail(&c, -5, "Failed to write install metadata");

    mkt_fetch_pkg(&c);
    mkt_mark_done(service, request->id);

    c.res->success = true;
    c.res->message = AIRY_STRDUP("Agent installed successfully");
    c.res->installed_version = AIRY_STRDUP(c.ver);
    c.res->install_path = AIRY_STRDUP(c.dir);
    c.res->error_code = 0;

    *result = c.res;
    mkt_snap_free(&c);
    return 0;
}

int market_service_install_skill(market_service_t *service, const install_request_t *request,
                                 install_result_t **result)
{
    int rc = mkt_ready(service, request, result, "market_service_install_skill");
    if (rc != 0)
        return rc;

    install_result_t *res = mkt_res_new("market_service_install_skill", "skill install result");
    if (!res)
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate skill install result");

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
    size_t idx = mkt_locate(service, agent_id);
    if (idx != (size_t)-1) {
        const char *storage = service->config.storage_path ? service->config.storage_path :
                                                             market_default_storage("agents");
        char install_dir[1024];
        snprintf(install_dir, sizeof(install_dir), "%s/%s", storage, agent_id);

        int rm_ret = recursive_remove(install_dir);
        if (rm_ret != 0 && errno != ENOENT) {
            SVC_LOG_WARN("Failed to remove install directory: %s (ret=%d)", install_dir, rm_ret);
        }

        service->agents[idx]->status = AGENT_STATUS_DISABLED;
        airy_mtx_unlock(&service->lock);
        return 0;
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
