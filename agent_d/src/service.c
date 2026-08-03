#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file service.c
 * @brief Agent 服务实现：Agent 派生/终止/调用/列表
 *
 * 从 gateway/src/utils/syscall_router.c 抽离的 g_runtime.agents[] 逻辑，
 * 重构为独立的、自包含的服务模块。守护进程 agent_d 持有 agent_service_t
 * 实例并通过 Unix socket 暴露 agent.* 命名空间方法。
 *
 * 设计要点：
 * - 自带哈希表（djb2 算法，与 syscall_router.c 同源但解耦）
 * - 线程安全：所有公共接口持锁
 * - Agent ID：32 字符十六进制（基于时间戳 + 计数器，无外部依赖）
 * - 终止不回收槽位：仅置 status=3，不压缩数组（与原实现一致）
 */

#include "service.h"

#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
/* Stage5+ 待办4：真实 spawn 所需的 POSIX 原语。
 * platform.h 已引入 unistd.h / signal.h / errno.h / fcntl.h / sys/types.h，
 * 这里补充 waitpid 与 select 所在头文件。 */
#include <sys/select.h>
#include <sys/wait.h>
#endif

#define AGENT_DEFAULT_MAX_AGENTS 256

#if AIRY_PLATFORM_POSIX
/* ==================== 真实 spawn 辅助（POSIX） ==================== */

/* invoke 读响应超时（秒），与设计要求 60s 一致 */
#define AGENT_INVOKE_TIMEOUT_S 60
/* spawn 后等待子进程 ready 的超时（秒）。Python runner 冷启动含依赖
 * 导入，典型 2~5s；15s 容忍慢速环境，超时即判定 spawn 失败（P0-2）。 */
#define AGENT_SPAWN_READY_TIMEOUT_S 15
/* 单行响应最大长度（与 MAX_BUFFER 65536 对齐） */
#define AGENT_RESP_BUF_SIZE 65536

/* 向 fd 写入全部字节（处理 EINTR 与短写）。
 * 返回 0 成功，-1 失败（含 EPIPE — 子进程已退出）。 */
static int agent_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

/* 带超时地从 fd 读取一行（以 '\n' 结尾）。
 * 成功时 buf 中存放不含 '\n' 的 null 结尾字符串，返回 0；
 * 超时或 EOF 或出错返回 -1。逐字节读取以避免跨行缓冲。 */
static int agent_read_line_timeout(int fd, char *buf, size_t buf_size, int timeout_s)
{
    if (buf_size < 2)
        return -1;
    fd_set rfds;
    struct timeval tv;
    size_t pos = 0;
    while (pos < buf_size - 1) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = timeout_s;
        tv.tv_usec = 0;
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rv == 0)
            return -1; /* 超时 */
        ssize_t n = read(fd, buf + pos, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* EOF — 子进程已关闭 stdout */
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            return 0;
        }
        pos++;
    }
    buf[buf_size - 1] = '\0';
    return 0;
}

/* P0-1：从 agent_d 可执行文件位置反推 Airymax 仓库根目录。
 * 沿 /proc/self/exe 的目录逐级向上，找到含 sdk/sdk-python/agentrt/
 * __init__.py 的祖先目录即为仓库根。返回 0 成功，-1 未找到。 */
static int agent_find_repo_root(char *out, size_t size)
{
    char exe[AIRY_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return -1;
    exe[n] = '\0';

    char dir[AIRY_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", exe);
    for (;;) {
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            break; /* 已到文件系统根 */
        *slash = '\0';
        char probe[AIRY_PATH_MAX];
        snprintf(probe, sizeof(probe),
                 "%s/sdk/sdk-python/agentrt/__init__.py", dir);
        if (access(probe, F_OK) == 0) {
            snprintf(out, size, "%s", dir);
            return 0;
        }
    }
    return -1;
}

/* P0-1：构造 agent 子进程的 Python 搜索路径（注入 PYTHONPATH 的前缀）。
 * 基础为环境变量 AIRY_AGENTS_PYTHONPATH（若设置）；随后自动补充仓库根下
 * 的关键目录 sdk/sdk-python、ecosystem/agents、ecosystem/openlab（存在且
 * 尚未包含时）。历史缺失 sdk/sdk-python 导致子进程无法导入 agentrt.syscall，
 * SyscallProxy 记忆持久化从未生效。结果写入 buf，始终以 null 结尾。 */
static void agent_build_agents_pypath(char *buf, size_t size)
{
    char merged[8192];
    size_t pos = 0;
    merged[0] = '\0';

    const char *custom = getenv("AIRY_AGENTS_PYTHONPATH");
    if (custom && custom[0] != '\0') {
        int n = snprintf(merged + pos, sizeof(merged) - pos, "%s", custom);
        if (n > 0 && (size_t)n < sizeof(merged) - pos)
            pos += (size_t)n;
    }

    char root[AIRY_PATH_MAX];
    if (agent_find_repo_root(root, sizeof(root)) == 0) {
        static const char *const suffixes[] = {
            "/sdk/sdk-python",
            "/ecosystem/agents",
            "/ecosystem/openlab",
        };
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            char probe[AIRY_PATH_MAX];
            snprintf(probe, sizeof(probe), "%s%s", root, suffixes[i]);
            if (access(probe, F_OK) != 0)
                continue; /* 该目录不存在（如裁剪安装），跳过 */
            int n;
            if (pos > 0 && merged[pos - 1] != ':')
                n = snprintf(merged + pos, sizeof(merged) - pos,
                             ":%s", probe);
            else
                n = snprintf(merged + pos, sizeof(merged) - pos,
                             "%s", probe);
            if (n > 0 && (size_t)n < sizeof(merged) - pos)
                pos += (size_t)n;
        }
    }

    snprintf(buf, size, "%s", merged);
}

/* 从 spec JSON 中提取 language 字段，默认 "python"。
 * 写入 buf（size 字节），始终以 null 结尾。 */
static void spec_get_language(const char *spec, char *buf, size_t size)
{
    if (!spec || size == 0) {
        if (size > 0) buf[0] = '\0';
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

/* 解析 Rust agent binary 路径。
 * 优先级：spec.binary_path > ${AIRY_RUST_AGENT_DIR}/<role>_agent。
 * 结果写入 out_path（AIRY_PATH_MAX 字节），始终以 null 结尾。 */
static void spec_resolve_rust_binary(const char *spec, const char *agent_id,
                                      char *out_path)
{
    if (!spec || !out_path) {
        if (out_path) out_path[0] = '\0';
        return;
    }
    cJSON *root = cJSON_Parse(spec);
    if (!root) {
        out_path[0] = '\0';
        return;
    }
    /* 1. spec.binary_path 优先 */
    cJSON *bin = cJSON_GetObjectItem(root, "binary_path");
    if (bin && cJSON_IsString(bin) && bin->valuestring[0] != '\0') {
        snprintf(out_path, AIRY_PATH_MAX, "%s", bin->valuestring);
        cJSON_Delete(root);
        return;
    }
    /* 2. ${AIRY_RUST_AGENT_DIR}/<role>_agent */
    cJSON *role = cJSON_GetObjectItem(root, "role");
    const char *role_str = (role && cJSON_IsString(role)) ? role->valuestring : "unknown";
    const char *agent_dir = getenv("AIRY_RUST_AGENT_DIR");
    if (agent_dir && agent_dir[0] != '\0') {
        snprintf(out_path, AIRY_PATH_MAX, "%s/%s_agent", agent_dir, role_str);
    } else {
        out_path[0] = '\0';
    }
    cJSON_Delete(root);
    /* 仅警告：不影响 agent 生命周期，下次 invoke 会检测到子进程未启动而回退 */
    (void)agent_id;
}

/* fork Agent runner 子进程，建立双向 stdin/stdout 管道。
 * 根据 spec.language 选择启动方式：
 *   - "python"（默认）：python3 -m airymax_agents.runner --spec <spec>
 *   - "rust"：<binary_path> --spec <spec>
 * 使用 execvp（不经过 shell，无注入风险）。
 * 成功返回 0，out_pid/out_stdin/out_stdout 写入句柄；失败返回 -1。
 * stderr 重定向到 ${AIRY_RUNTIME_DIR}/agent_<agent_id>.log 便于调试。 */
static int agent_spawn_child(const char *spec, const char *agent_id,
                              pid_t *out_pid, int *out_stdin, int *out_stdout)
{
    char lang[16] = {0};
    spec_get_language(spec, lang, sizeof(lang));

    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) < 0)
        return -1;
    if (pipe(stdout_pipe) < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* 子进程：重定向 stdin/stdout 到管道，关闭父进程持有的端 */
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0)
            _exit(127);
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        /* stderr → 日志文件（best-effort，失败则继承父进程 stderr） */
        char log_path[AIRY_PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/agent_%s.log",
                 AIRY_RUNTIME_DIR, agent_id);
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        if (strcmp(lang, "rust") == 0) {
            /* Rust agent：binary_path 或 ${AIRY_RUST_AGENT_DIR}/<role>_agent */
            char bin_path[AIRY_PATH_MAX] = {0};
            spec_resolve_rust_binary(spec, agent_id, bin_path);
            if (bin_path[0] == '\0') {
                /* 无有效 binary 路径 → 回退到 Python */
                SVC_LOG_WARN("Rust agent binary path empty, fallback to python3: agent_id=%s",
                             agent_id);
                goto fallback_python;
            }
            char *argv[] = {
                bin_path,
                (char *)"--spec",
                (char *)spec,
                NULL,
            };
            execvp(bin_path, argv);
            /* Rust binary 可能不存在或无执行权限，回退到 Python */
            SVC_LOG_WARN("execvp Rust agent failed, fallback to python3: binary=%s, agent_id=%s",
                         bin_path, agent_id);
        }

fallback_python:
        /* Python runner（默认 & 回退路径）
         *
         * 注入 Agent Python 运行时路径：airymax_agents 及其依赖（openlab、
         * agentrt SDK）不在系统 site-packages，需通过 PYTHONPATH（冒号分隔）
         * 指定搜索路径，否则子进程启动即失败（历史 P0-3 变体：
         * ModuleNotFoundError）。
         *
         * P0-1：路径自动补全。基础为 AIRY_AGENTS_PYTHONPATH（若显式设置），
         * 再按 agent_d 可执行文件位置反推仓库根，自动补上 sdk/sdk-python、
         * ecosystem/agents、ecosystem/openlab 三个关键目录。修复历史上
         * 缺失 sdk/sdk-python 导致 SyscallProxy 记忆持久化从未生效的问题。
         * 未找到仓库根（生产裁剪安装）时仅使用显式环境变量，兼容旧部署。 */
        {
            char agents_pypath[8192];
            agent_build_agents_pypath(agents_pypath, sizeof(agents_pypath));
            if (agents_pypath[0] != '\0') {
                const char *cur = getenv("PYTHONPATH");
                if (cur && cur[0] != '\0') {
                    char merged[9216];
                    snprintf(merged, sizeof(merged), "%s:%s",
                             agents_pypath, cur);
                    setenv("PYTHONPATH", merged, 1);
                } else {
                    setenv("PYTHONPATH", agents_pypath, 1);
                }
            }
            char *argv[] = {
                (char *)"python3",
                (char *)"-m",
                (char *)"airymax_agents.runner",
                (char *)"--spec",
                (char *)spec,
                NULL,
            };
            execvp("python3", argv);
        }
        /* 仅 execvp 失败才到达此处 */
        _exit(127);
    }

    /* 父进程：保留 stdin 写端与 stdout 读端 */
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    *out_pid = pid;
    *out_stdin = stdin_pipe[1];
    *out_stdout = stdout_pipe[0];
    return 0;
}

/* 回收子进程并关闭管道句柄（terminate / invoke 失败 / destroy 调用）。
 * 先 SIGTERM，等待最多 2 秒，仍不退出则 SIGKILL，最后 waitpid 回收僵尸。 */
static void agent_kill_and_reap(pid_t *pid_ptr, int *stdin_ptr, int *stdout_ptr)
{
    pid_t pid = *pid_ptr;
    if (pid <= 0)
        return;

    kill(pid, SIGTERM);
    /* 非阻塞轮询 2 秒等待退出 */
    for (int i = 0; i < 20; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0)
            break;
        /* r == 0 表示仍未退出，休眠 100ms */
        struct timespec ts = {0, 100 * 1000000L};
        nanosleep(&ts, NULL);
    }
    /* 仍存活则 SIGKILL */
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    if (*stdin_ptr >= 0) {
        close(*stdin_ptr);
        *stdin_ptr = -1;
    }
    if (*stdout_ptr >= 0) {
        close(*stdout_ptr);
        *stdout_ptr = -1;
    }
    *pid_ptr = -1;
}
#endif /* AIRY_PLATFORM_POSIX */
#define AGENT_ID_LEN 33          /* 32 字符 + '\0' */
#define AGENT_HASH_LOAD_FACTOR 4 /* capacity = max_agents * 4 */

/* ==================== 内部哈希表 ==================== */

static unsigned long agent_hash_fn(const char *str)
{
    /* djb2 — 与 syscall_router.c::hash_fn 同算法，避免符号碰撞 */
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static int agent_ht_init(agent_hash_table_t *ht, size_t capacity)
{
    if (!ht || capacity == 0)
        return AIRY_ERR_INVALID_PARAM;

    ht->entries = (agent_hash_entry_t *)AIRY_CALLOC(capacity, sizeof(agent_hash_entry_t));
    if (!ht->entries) {
        ht->capacity = 0;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    ht->capacity = capacity;
    ht->count = 0;
    return AIRY_SUCCESS;
}

static void agent_ht_destroy(agent_hash_table_t *ht)
{
    if (!ht || !ht->entries)
        return;
    for (size_t i = 0; i < ht->capacity; i++) {
        AIRY_FREE(ht->entries[i].key);
    }
    AIRY_FREE(ht->entries);
    ht->entries = NULL;
    ht->capacity = 0;
    ht->count = 0;
}

static int agent_ht_insert(agent_hash_table_t *ht, const char *key, size_t index)
{
    if (!ht || !ht->entries || !key)
        return AIRY_ERR_INVALID_PARAM;
    if (ht->count >= ht->capacity * 3 / 4)
        return AIRY_ERR_OUT_OF_MEMORY;

    unsigned long h = agent_hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            ht->entries[pos].key = AIRY_STRDUP(key);
            if (!ht->entries[pos].key)
                return AIRY_ERR_OUT_OF_MEMORY;
            ht->entries[pos].index = index;
            ht->entries[pos].occupied = 1;
            ht->count++;
            return AIRY_SUCCESS;
        }
    }
    return AIRY_ERR_OUT_OF_MEMORY;
}

static ssize_t agent_ht_lookup(agent_hash_table_t *ht, const char *key)
{
    if (!ht || !ht->entries || !key || ht->count == 0)
        return -1;

    unsigned long h = agent_hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied)
            return -1;
        if (strcmp(ht->entries[pos].key, key) == 0)
            return (ssize_t)ht->entries[pos].index;
    }
    return -1;
}

/* ==================== Agent ID 生成 ==================== */

static void agent_generate_agent_id(char *buf, size_t buf_size)
{
    /* 32 字符十六进制：8 字符时间戳 + 8 字符计数器 + 16 字符随机
     * 无外部 libuuid 依赖，保证 daemon 可独立运行 */
    static uint64_t counter = 0;
    static airy_mtx_t counter_lock;
    static int counter_initialized = 0;

    if (!counter_initialized) {
        airy_mtx_init(&counter_lock);
        counter = (uint64_t)time(NULL) & 0xFFFFFFFF;
        counter_initialized = 1;
    }

    airy_mtx_lock(&counter_lock);
    uint64_t c = counter++;
    airy_mtx_unlock(&counter_lock);

    uint64_t t = (uint64_t)time(NULL);
    /* xorshift 简单 PRNG，基于时间 + 计数器 */
    uint64_t r = t ^ (c * 0x9E3779B97F4A7C15ULL);
    r ^= r << 13;
    r ^= r >> 7;
    r ^= r << 17;

    if (buf_size < AGENT_ID_LEN)
        return;
    snprintf(buf, AGENT_ID_LEN, "%08lx%08lx%016lx",
             (unsigned long)(t & 0xFFFFFFFFu),
             (unsigned long)(c & 0xFFFFFFFFu),
             (unsigned long)(r & 0xFFFFFFFFFFFFFFFFULL));
}

/* ==================== 公共接口实现 ==================== */

agent_service_t *agent_service_create(size_t max_agents)
{
    if (max_agents == 0)
        max_agents = AGENT_DEFAULT_MAX_AGENTS;

    agent_service_t *svc = (agent_service_t *)AIRY_CALLOC(1, sizeof(agent_service_t));
    if (!svc)
        return NULL;

    svc->max_agents = max_agents;
    svc->agents = (agent_entry_internal_t *)AIRY_CALLOC(max_agents,
                                                          sizeof(agent_entry_internal_t));
    if (!svc->agents) {
        AIRY_FREE(svc);
        return NULL;
    }

    if (agent_ht_init(&svc->agent_index, max_agents * AGENT_HASH_LOAD_FACTOR) != AIRY_SUCCESS) {
        AIRY_FREE(svc->agents);
        AIRY_FREE(svc);
        return NULL;
    }

    airy_mtx_init(&svc->lock);
    svc->agent_count = 0;
    svc->initialized = 1;
#if AIRY_PLATFORM_POSIX
    /* fork 子进程后，子进程退出会导致管道写端收到 SIGPIPE。
     * 忽略该信号，write 改为返回 EPIPE 错误，由调用方处理。 */
    signal(SIGPIPE, SIG_IGN);
#endif
    SVC_LOG_INFO("Agent service created (max_agents=%zu)", max_agents);
    return svc;
}

void agent_service_destroy(agent_service_t *svc)
{
    if (!svc)
        return;

    airy_mtx_lock(&svc->lock);
    for (size_t i = 0; i < svc->agent_count; i++) {
        AIRY_FREE(svc->agents[i].agent_id);
        AIRY_FREE(svc->agents[i].spec);
    }
    AIRY_FREE(svc->agents);
    agent_ht_destroy(&svc->agent_index);
    svc->agent_count = 0;
    svc->max_agents = 0;
    svc->initialized = 0;
    airy_mtx_unlock(&svc->lock);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

int agent_service_spawn(agent_service_t *svc, const char *spec,
                          char **out_agent_id)
{
    if (!svc || !svc->initialized || !spec || spec[0] == '\0' || !out_agent_id)
        return AIRY_ERR_INVALID_PARAM;

    *out_agent_id = NULL;

    airy_mtx_lock(&svc->lock);

    if (svc->agent_count >= svc->max_agents) {
        airy_mtx_unlock(&svc->lock);
        SVC_LOG_WARN("Agent service full (count=%zu)", svc->agent_count);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t idx = svc->agent_count;
    agent_entry_internal_t *agent = &svc->agents[idx];

    char id_buf[AGENT_ID_LEN];
    agent_generate_agent_id(id_buf, sizeof(id_buf));
    agent->agent_id = AIRY_STRDUP(id_buf);
    agent->spec = AIRY_STRDUP(spec);
    if (!agent->agent_id || !agent->spec) {
        AIRY_FREE(agent->agent_id);
        AIRY_FREE(agent->spec);
        agent->agent_id = NULL;
        agent->spec = NULL;
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    agent->status = 1;
    agent->spawned_at = (uint64_t)time(NULL);

#if AIRY_PLATFORM_POSIX
    agent->child_pid = -1;
    agent->stdin_fd = -1;
    agent->stdout_fd = -1;
    agent->last_active = (uint64_t)time(NULL);

    /* P0-2：真实派生并校验子进程存活，不再静默回退 stub。
     * AIRY_AGENT_NO_SPAWN=1 时跳过 fork（单元测试确定性模式）：视为成功
     * 注册但无子进程，invoke 会返回明确错误而非假成功。 */
    const char *no_spawn_env = getenv("AIRY_AGENT_NO_SPAWN");
    int spawn_disabled = (no_spawn_env && no_spawn_env[0] != '\0' &&
                          strcmp(no_spawn_env, "0") != 0);

    pid_t child_pid = -1;
    int child_sin = -1, child_sout = -1;
    if (spawn_disabled) {
        SVC_LOG_WARN("Agent spawn skipped (AIRY_AGENT_NO_SPAWN): agent_id=%s",
                     agent->agent_id);
    } else if (agent_spawn_child(spec, agent->agent_id,
                                 &child_pid, &child_sin, &child_sout) == 0) {
        /* 等待子进程 ready 确认存活；失败则回收子进程并判定 spawn 失败 */
        char ready_buf[512];
        int ready_rc = agent_read_line_timeout(child_sout, ready_buf,
                                               sizeof(ready_buf),
                                               AGENT_SPAWN_READY_TIMEOUT_S);
        int alive = 0;
        if (ready_rc == 0) {
            cJSON *r = cJSON_Parse(ready_buf);
            if (r) {
                cJSON *ready_item = cJSON_GetObjectItem(r, "ready");
                if (ready_item && cJSON_IsTrue(ready_item))
                    alive = 1;
                cJSON_Delete(r);
            }
        }
        if (alive) {
            agent->child_pid = child_pid;
            agent->stdin_fd = child_sin;
            agent->stdout_fd = child_sout;
            SVC_LOG_INFO("Agent child spawned: agent_id=%s, pid=%d",
                         agent->agent_id, (int)child_pid);
        } else {
            SVC_LOG_WARN("Agent child not ready, spawn rejected: agent_id=%s, resp=%s",
                         agent->agent_id,
                         ready_rc == 0 ? ready_buf : "(timeout/eof)");
            pid_t dead_pid = child_pid;
            int dead_sin = child_sin, dead_sout = child_sout;
            agent_kill_and_reap(&dead_pid, &dead_sin, &dead_sout);
            child_pid = -1; /* 标记 spawn 失败，进入下方失败判定 */
        }
    } else {
        SVC_LOG_WARN("Agent child spawn failed: agent_id=%s", agent->agent_id);
    }

    if (child_pid <= 0 && !spawn_disabled) {
        /* 子进程未存活 → spawn 整体失败，不返回"幽灵"agent（无子进程却
         * 被视为成功，导致 invoke 静默回退 stub 假输出）。释放槽位资源。 */
        AIRY_FREE(agent->agent_id);
        AIRY_FREE(agent->spec);
        agent->agent_id = NULL;
        agent->spec = NULL;
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_SVC_NOT_READY;
    }
#endif

    /* 子进程存活确认后才注册到哈希表与计数 */
    int rc = agent_ht_insert(&svc->agent_index, agent->agent_id, idx);
    if (rc != AIRY_SUCCESS) {
#if AIRY_PLATFORM_POSIX
        if (agent->child_pid > 0) {
            agent_kill_and_reap(&agent->child_pid,
                                &agent->stdin_fd, &agent->stdout_fd);
        }
#endif
        AIRY_FREE(agent->agent_id);
        AIRY_FREE(agent->spec);
        agent->agent_id = NULL;
        agent->spec = NULL;
        airy_mtx_unlock(&svc->lock);
        return rc;
    }

    *out_agent_id = AIRY_STRDUP(agent->agent_id);
    svc->agent_count++;
    uint64_t total_agents = svc->agent_count;

    airy_mtx_unlock(&svc->lock);

    SVC_LOG_DEBUG("Agent spawn: agent_id=%s, total=%lu",
                  *out_agent_id, (unsigned long)total_agents);
    return AIRY_SUCCESS;
}

int agent_service_terminate(agent_service_t *svc, const char *agent_id)
{
    if (!svc || !svc->initialized || !agent_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);

    ssize_t idx = agent_ht_lookup(&svc->agent_index, agent_id);
    if (idx < 0 || (size_t)idx >= svc->agent_count) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }

#if AIRY_PLATFORM_POSIX
    if (svc->agents[idx].child_pid > 0) {
        agent_kill_and_reap(&svc->agents[idx].child_pid,
                            &svc->agents[idx].stdin_fd,
                            &svc->agents[idx].stdout_fd);
    }
#endif
    /* 仅置终止状态，不回收槽位（与原 syscall_router.c 实现一致） */
    svc->agents[idx].status = 3;

    airy_mtx_unlock(&svc->lock);

    SVC_LOG_DEBUG("Agent terminate: agent_id=%s", agent_id);
    return AIRY_SUCCESS;
}

int agent_service_invoke(agent_service_t *svc, const char *agent_id,
                          const char *input, size_t len, char **out_output)
{
    if (!svc || !svc->initialized || !agent_id || !out_output)
        return AIRY_ERR_INVALID_PARAM;

    *out_output = NULL;

    airy_mtx_lock(&svc->lock);

    ssize_t idx = agent_ht_lookup(&svc->agent_index, agent_id);
    if (idx < 0 || (size_t)idx >= svc->agent_count) {
        airy_mtx_unlock(&svc->lock);
        *out_output = AIRY_STRDUP("{\"error\":\"Agent not found\"}");
        return AIRY_ERR_NOT_FOUND;
    }

    if (svc->agents[idx].status != 1) {
        airy_mtx_unlock(&svc->lock);
        *out_output = AIRY_STRDUP("{\"error\":\"Agent not running\"}");
        return AIRY_ERR_STATE_ERROR;
    }

#if AIRY_PLATFORM_POSIX
    /* Stage5+ 待办4：真实子进程路径 — 通过 stdin/stdout 管道通信。
     * 协议：向子进程 stdin 写一行 JSON 请求，从 stdout 读一行 JSON 响应。
     * child_pid>0 且 stdin_fd>=0 表示有活跃子进程；否则走回退路径。 */
    if (svc->agents[idx].child_pid > 0 && svc->agents[idx].stdin_fd >= 0) {
        int sin_fd = svc->agents[idx].stdin_fd;
        int sout_fd = svc->agents[idx].stdout_fd;

        /* 构建请求 JSON: {"agent_id":..., "input":...} */
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "agent_id", agent_id);
        cJSON_AddStringToObject(req, "input", input ? input : "");
        char *req_str = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);

        if (!req_str) {
            airy_mtx_unlock(&svc->lock);
            *out_output = AIRY_STRDUP("{\"error\":\"failed to build request\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }

        /* 写请求到子进程 stdin（追加 '\n' 作为行分隔） */
        size_t req_len = strlen(req_str);
        char *write_buf = (char *)AIRY_MALLOC(req_len + 2);
        if (!write_buf) {
            AIRY_FREE(req_str);
            airy_mtx_unlock(&svc->lock);
            *out_output = AIRY_STRDUP("{\"error\":\"out of memory\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        __builtin_memcpy(write_buf, req_str, req_len);
        write_buf[req_len] = '\n';
        write_buf[req_len + 1] = '\0';
        AIRY_FREE(req_str);

        int wrc = agent_write_all(sin_fd, write_buf, req_len + 1);
        AIRY_FREE(write_buf);
        if (wrc != 0) {
            SVC_LOG_WARN("Agent invoke write failed, child unusable: agent_id=%s",
                         agent_id);
            agent_kill_and_reap(&svc->agents[idx].child_pid,
                                &svc->agents[idx].stdin_fd,
                                &svc->agents[idx].stdout_fd);
            goto invoke_fallback;
        }
        /* P0-3：成功向子进程写入请求视为活跃，更新空闲回收基准 */
        svc->agents[idx].last_active = (uint64_t)time(NULL);

        /* 从子进程 stdout 读响应行（带 60s 超时） */
        char *resp_buf = (char *)AIRY_MALLOC(AGENT_RESP_BUF_SIZE);
        if (!resp_buf) {
            airy_mtx_unlock(&svc->lock);
            *out_output = AIRY_STRDUP("{\"error\":\"out of memory\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        int rrc = agent_read_line_timeout(sout_fd, resp_buf,
                                          AGENT_RESP_BUF_SIZE, AGENT_INVOKE_TIMEOUT_S);
        if (rrc != 0) {
            AIRY_FREE(resp_buf);
            SVC_LOG_WARN("Agent invoke read failed, child unusable: agent_id=%s",
                         agent_id);
            agent_kill_and_reap(&svc->agents[idx].child_pid,
                                &svc->agents[idx].stdin_fd,
                                &svc->agents[idx].stdout_fd);
            goto invoke_fallback;
        }
        /* 收到响应同样刷新活跃时间 */
        svc->agents[idx].last_active = (uint64_t)time(NULL);

        /* 解析响应 JSON，提取 output 字段（runner.py 约定）。
         * 成功: {"success":true,"output":"..."}
         * 失败: {"success":false,"error":"..."} */
        cJSON *resp = cJSON_Parse(resp_buf);
        if (resp) {
            cJSON *success_item = cJSON_GetObjectItem(resp, "success");
            cJSON *err_item = cJSON_GetObjectItem(resp, "error");
            if (success_item && cJSON_IsFalse(success_item) && err_item && cJSON_IsString(err_item)) {
                *out_output = AIRY_STRDUP(err_item->valuestring);
            } else {
                cJSON *output_item = cJSON_GetObjectItem(resp, "output");
                if (output_item && cJSON_IsString(output_item)) {
                    *out_output = AIRY_STRDUP(output_item->valuestring);
                } else {
                    *out_output = AIRY_STRDUP(resp_buf);
                }
            }
            cJSON_Delete(resp);
        } else {
            /* 响应非合法 JSON — 原样返回，调用方自行判断 */
            *out_output = AIRY_STRDUP(resp_buf);
        }
        AIRY_FREE(resp_buf);

        airy_mtx_unlock(&svc->lock);
        SVC_LOG_DEBUG("Agent invoke via child: agent_id=%s", agent_id);
        return AIRY_SUCCESS;
    }
#endif

invoke_fallback:
    /* P0-2：无子进程或通信失败 → 返回明确错误，不再伪造"invocation
     * processed"假成功。上层（taskflow/SDK）据此感知 agent 不可用，
     * 避免静默吞掉故障。 */
    (void)input;
    (void)len;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "agent_id", agent_id);
    cJSON_AddStringToObject(result, "error",
                            "agent child process unavailable");
    *out_output = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    airy_mtx_unlock(&svc->lock);

    SVC_LOG_WARN("Agent invoke fallback (no child): agent_id=%s", agent_id);
    return AIRY_ERR_SVC_NOT_READY;
}

int agent_service_list(agent_service_t *svc, char ***out_agent_ids,
                         size_t *out_count)
{
    if (!svc || !svc->initialized || !out_agent_ids || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_agent_ids = NULL;
    *out_count = 0;

    airy_mtx_lock(&svc->lock);

    if (svc->agent_count == 0) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_SUCCESS;
    }

    char **ids = (char **)AIRY_CALLOC(svc->agent_count, sizeof(char *));
    if (!ids) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < svc->agent_count; i++) {
        ids[i] = AIRY_STRDUP(svc->agents[i].agent_id);
        if (!ids[i]) {
            for (size_t j = 0; j < i; j++)
                AIRY_FREE(ids[j]);
            AIRY_FREE(ids);
            airy_mtx_unlock(&svc->lock);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
    }

    *out_agent_ids = ids;
    *out_count = svc->agent_count;

    airy_mtx_unlock(&svc->lock);

    SVC_LOG_DEBUG("Agent list: count=%zu", *out_count);
    return AIRY_SUCCESS;
}

size_t agent_service_count(agent_service_t *svc)
{
    if (!svc || !svc->initialized)
        return 0;
    airy_mtx_lock(&svc->lock);
    size_t c = svc->agent_count;
    airy_mtx_unlock(&svc->lock);
    return c;
}

int agent_service_reap_idle(agent_service_t *svc, uint64_t max_idle_s)
{
    if (!svc || !svc->initialized)
        return AIRY_ERR_INVALID_PARAM;

    uint64_t now = (uint64_t)time(NULL);
    size_t reaped = 0;

    airy_mtx_lock(&svc->lock);
#if AIRY_PLATFORM_POSIX
    for (size_t i = 0; i < svc->agent_count; i++) {
        agent_entry_internal_t *a = &svc->agents[i];
        if (a->status != 1 || a->child_pid <= 0)
            continue;
        if (now <= a->last_active || (now - a->last_active) < max_idle_s)
            continue;
        SVC_LOG_INFO("Agent idle reclaimed: agent_id=%s, idle=%llus",
                     a->agent_id,
                     (unsigned long long)(now - a->last_active));
        agent_kill_and_reap(&a->child_pid, &a->stdin_fd, &a->stdout_fd);
        a->status = 3;
        reaped++;
    }
#endif
    airy_mtx_unlock(&svc->lock);

    if (reaped > 0)
        SVC_LOG_INFO("Agent idle reclaim done: reaped=%zu", reaped);
    return AIRY_SUCCESS;
}

void agent_service_list_free(char **agent_ids, size_t count)
{
    if (!agent_ids)
        return;
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(agent_ids[i]);
    }
    AIRY_FREE(agent_ids);
}
