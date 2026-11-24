// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_child.c
 * @brief Agent service child-process domain, platform-neutral part: timeout
 *        budgets, spec parsing and agent log retention. The transport itself
 *        lives in service_child_posix.c and service_child_win.c (0.1.21 P0-1:
 *        Windows had no child transport at all, so spawn answered
 *        AIRY_ERR_NOT_SUPPORTED and orchestration could not run there).
 */

#include "io.h"

#include "agent_service_internal.h"

#include "airy_dirent.h"
#include "platform.h"

#include <sys/stat.h>

/* invoke read-response timeout (seconds). Default 300s (5 min) covers real LLM
 * calls; overridable via AIRY_AGENT_INVOKE_TIMEOUT_S. */
#define AGENT_INVOKE_TIMEOUT_S 300
/* spawn ready timeout (seconds) after starting the child. Python runner cold
 * start includes dependency imports, typically 2-5s; the default 15s tolerates
 * slow environments and treats timeout as spawn failure (P0-2). Overridable via
 * AIRY_AGENT_SPAWN_TIMEOUT_S. */
#define AGENT_SPAWN_READY_TIMEOUT_S 15

/* Parse a positive integer override from the environment. */
static int env_int(const char *name, int dflt)
{
    const char *env = getenv(name);
    if (env && env[0] != '\0') {
        long v = strtol(env, NULL, 10);
        if (v > 0)
            return (int)v;
    }
    return dflt;
}

/* Budget parsers are platform-neutral (getenv/strtol only): the platform-
 * neutral invoke entry points reference them on every platform, so they must
 * not live inside the POSIX-only section (0.1.17: MSVC LNK2001). */
int agent_invoke_timeout_s(void)
{
    return env_int("AIRY_AGENT_INVOKE_TIMEOUT_S", AGENT_INVOKE_TIMEOUT_S);
}

int agent_spawn_ready_timeout_s(void)
{
    return env_int("AIRY_AGENT_SPAWN_TIMEOUT_S", AGENT_SPAWN_READY_TIMEOUT_S);
}

/* Extract the language field from the spec JSON, default "python".
 * Writes to buf (size bytes), always null-terminated. */
void agent_spec_language(const char *spec, char *buf, size_t size)
{
    if (!buf || size == 0)
        return;
    if (!spec) {
        buf[0] = '\0';
        return;
    }
    cJSON *root = cJSON_Parse(spec);
    if (!root) {
        snprintf(buf, size, "python");
        return;
    }
    cJSON *lang = cJSON_GetObjectItem(root, "language");
    if (lang && cJSON_IsString(lang) && lang->valuestring[0] != '\0') {
        snprintf(buf, size, "%s", lang->valuestring);
    } else {
        snprintf(buf, size, "python");
    }
    cJSON_Delete(root);
}

/* Resolve the Rust agent binary path.
 * Priority: spec.binary_path > ${AIRY_RUST_AGENT_DIR}/<role>_agent.
 * Result written to out_path (cap bytes), always null-terminated. */
void agent_spec_rust_binary(const char *spec, char *out_path, size_t cap)
{
    if (!out_path || cap == 0)
        return;
    out_path[0] = '\0';
    if (!spec)
        return;

    cJSON *root = cJSON_Parse(spec);
    if (!root)
        return;

    cJSON *bin = cJSON_GetObjectItem(root, "binary_path");
    if (bin && cJSON_IsString(bin) && bin->valuestring[0] != '\0') {
        snprintf(out_path, cap, "%s", bin->valuestring);
        cJSON_Delete(root);
        return;
    }
    /* 2. ${AIRY_RUST_AGENT_DIR}/<role>_agent[.exe] */
    cJSON *role = cJSON_GetObjectItem(root, "role");
    const char *role_str = (role && cJSON_IsString(role)) ? role->valuestring : "unknown";
    const char *agent_dir = getenv("AIRY_RUST_AGENT_DIR");
    if (agent_dir && agent_dir[0] != '\0') {
#ifdef _WIN32
        snprintf(out_path, cap, "%s/%s_agent.exe", agent_dir, role_str);
#else
        snprintf(out_path, cap, "%s/%s_agent", agent_dir, role_str);
#endif
    }
    cJSON_Delete(root);
}

/* agent 子进程日志保留策略（2026-08-25）────────────────────────
 * agent_<id>.log 随每次 spawn 创建，长期运行会无限堆积（实测数百个、
 * 数十 MB 量级）。每次 spawn 成功后清理，仅保留最近 AGENT_LOG_KEEP_MAX
 * 个（按 mtime，最旧优先删除），防止日志目录失控（AIRY_LOG_DIR 现
 * 统一收敛于 $AIRY_HOME/data/agentrt/logs）。 */
#define AGENT_LOG_KEEP_MAX 64
#define AGENT_LOG_SCAN_MAX 512

/* agent_<id>.log 的落盘路径；stderr 重定向与清理都以它为唯一口径。 */
void agent_child_log_path(const char *agent_id, char *out_path, size_t cap)
{
    if (!out_path || cap == 0)
        return;
    const char *dir = airy_log_dir();
    if (!dir || !dir[0])
        dir = ".";
    snprintf(out_path, cap, "%s/agent_%s.log", dir, agent_id ? agent_id : "unknown");
}

/* 常规文件判定：MSVC 的 UCRT 不提供 S_ISREG，用 _S_IFMT/_S_IFREG 等价替换。 */
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

/* 是否 agent_*.log 常规文件；命中时回填完整路径与 mtime，返回 1。 */
static int prune_match(const char *logdir, const char *name, char *full, size_t cap,
                       time_t *mtime)
{
    if (strncmp(name, "agent_", 6) != 0)
        return 0;
    size_t nlen = strlen(name);
    if (nlen < 5 || strcmp(name + nlen - 4, ".log") != 0)
        return 0;

    snprintf(full, cap, "%s/%s", logdir, name);
#ifdef _WIN32
    struct _stat st;
    if (_stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
#else
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
#endif
    *mtime = st.st_mtime;
    return 1;
}

/* 按 mtime 升序插入（最旧在前，便于批量删除）。 */
static void prune_insert(char **paths, time_t *mtimes, size_t *count, char *copy, time_t mtime)
{
    size_t pos = *count;
    while (pos > 0 && mtimes[pos - 1] > mtime) {
        paths[pos] = paths[pos - 1];
        mtimes[pos] = mtimes[pos - 1];
        pos--;
    }
    paths[pos] = copy;
    mtimes[pos] = mtime;
    (*count)++;
}

/* 扫描日志目录并收集 agent_*.log，返回条数（上限 AGENT_LOG_SCAN_MAX）。 */
static size_t prune_scan(const char *logdir, char **paths, time_t *mtimes)
{
    DIR *dir = opendir(logdir);
    if (!dir)
        return 0;

    size_t count = 0;
    struct dirent *entry;
    while (count < AGENT_LOG_SCAN_MAX && (entry = readdir(dir)) != NULL) {
        char full[AIRY_PATH_MAX];
        time_t mtime = 0;
        if (!prune_match(logdir, entry->d_name, full, sizeof(full), &mtime))
            continue;
        char *copy = AIRY_STRDUP(full);
        if (!copy)
            continue;
        prune_insert(paths, mtimes, &count, copy, mtime);
    }
    closedir(dir);
    return count;
}

void agent_child_logs_prune(void)
{
    const char *logdir = airy_log_dir();
    if (!logdir || !logdir[0])
        return;

    char *paths[AGENT_LOG_SCAN_MAX] = {0};
    time_t mtimes[AGENT_LOG_SCAN_MAX] = {0};
    size_t count = prune_scan(logdir, paths, mtimes);

    if (count <= (size_t)AGENT_LOG_KEEP_MAX) {
        for (size_t i = 0; i < count; i++)
            AIRY_FREE(paths[i]);
        return;
    }

    size_t excess = count - (size_t)AGENT_LOG_KEEP_MAX;
    for (size_t i = 0; i < excess; i++) {
        remove(paths[i]);
        AIRY_FREE(paths[i]);
    }
    for (size_t i = excess; i < count; i++)
        AIRY_FREE(paths[i]);
}
