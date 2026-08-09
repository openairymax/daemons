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

/* 默认最大并发 Agent 数：支持上千 Agent 并行（用户设计预期）。
 * 可通过 AIRY_MAX_AGENTS 环境变量或 daemon 配置 max_agents 覆盖（上限 65535）。 */
#define AGENT_DEFAULT_MAX_AGENTS 10000

#if AIRY_PLATFORM_POSIX
/* ==================== 真实 spawn 辅助（POSIX） ==================== */

/* invoke 读响应超时（秒）。默认 300s（5 分钟）覆盖真实 LLM 调用；
 * 可通过 AIRY_AGENT_INVOKE_TIMEOUT_S 覆盖。 */
#define AGENT_INVOKE_TIMEOUT_S 300
/* spawn 后等待子进程 ready 的超时（秒）。Python runner 冷启动含依赖
 * 导入，典型 2~5s；默认 15s 容忍慢速环境，超时即判定 spawn 失败（P0-2）。
 * 可通过 AIRY_AGENT_SPAWN_TIMEOUT_S 覆盖。 */
#define AGENT_SPAWN_READY_TIMEOUT_S 15
/* 单行响应最大长度（与 MAX_BUFFER 65536 对齐） */
#define AGENT_RESP_BUF_SIZE 65536

/* 运行时可配置超时：env 优先，缺省用编译期默认值 */
static int agent_invoke_timeout_s(void)
{
    const char *env = getenv("AIRY_AGENT_INVOKE_TIMEOUT_S");
    if (env && env[0] != '\0') {
        long v = strtol(env, NULL, 10);
        if (v > 0)
            return (int)v;
    }
    return AGENT_INVOKE_TIMEOUT_S;
}

static int agent_spawn_ready_timeout_s(void)
{
    const char *env = getenv("AIRY_AGENT_SPAWN_TIMEOUT_S");
    if (env && env[0] != '\0') {
        long v = strtol(env, NULL, 10);
        if (v > 0)
            return (int)v;
    }
    return AGENT_SPAWN_READY_TIMEOUT_S;
}

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

/* 带超时地从 fd 读取一行（以 '\n' 结尾），支持取消令牌短轮询（改进1）。
 * 成功时 buf 中存放不含 '\n' 的 null 结尾字符串。
 * 返回：0=完整行；1=缓冲满截断（buf 已 null 结尾，调用方需标记）；
 *      -1=超时/EOF/出错；-2=取消（token 命中）。
 * select 以 200ms 短片轮询，保证取消判定粒度；逐字节读取避免跨行缓冲。 */
static int agent_read_line_timeout_ex(int fd, char *buf, size_t buf_size, int timeout_s,
                                      airy_cancel_token_t *token)
{
    if (buf_size < 2)
        return -1;
    uint64_t deadline_ms = airy_time_ms() + (uint64_t)(timeout_s > 0 ? timeout_s : 300) * 1000U;
    size_t pos = 0;
    while (pos < buf_size - 1) {
        /* 取消检查：命中立即中止（与超时 -1 区分，返回 -2） */
        if (token && airy_cancel_token_is_canceled(token))
            return -2;
        uint64_t now = airy_time_ms();
        if (now >= deadline_ms)
            return -1; /* 超时 */
        uint32_t remain = (uint32_t)(deadline_ms - now);
        if (remain > 200U)
            remain = 200U; /* 短片轮询：取消粒度 200ms */

        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = remain / 1000U;
        tv.tv_usec = (remain % 1000U) * 1000U;
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rv == 0)
            continue; /* 本片超时：回到循环顶部复查 token 与总 deadline */
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
    return 1; /* 缓冲满：截断，明确标记 */
}

/* 无取消令牌版本（既有语义不变，token=NULL） */
static int agent_read_line_timeout(int fd, char *buf, size_t buf_size, int timeout_s)
{
    return agent_read_line_timeout_ex(fd, buf, buf_size, timeout_s, NULL);
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
        /* 改进1：自建进程组（setpgid(0,0)），使终止可按组 SIGTERM/SIGKILL
         * 级联到 runner 派生的全部子进程（优雅终止整棵进程树）。 */
        setpgid(0, 0);
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0)
            _exit(127);
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        /* stderr → 日志文件（best-effort，失败则继承父进程 stderr）。
         * 日志收敛到 AIRY_HOME/run（与 socket 同目录，便于排查）。 */
        char log_path[AIRY_PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/agent_%s.log",
                 airy_runtime_dir(), agent_id);
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
         * 依赖解析：airymax_agents / openlab / agentrt SDK 通过标准 Python
         * 包安装（pip install -e，见 ecosystem 三包 packaging）解析，不再注入
         * PYTHONPATH 或从可执行文件位置反推源码树（历史 P0-1 机制的移除，
         * 参见 docs-closed/agentrt/01-designs/_design_0.1.1/06-agent-gateway-wiring.md §3.1）。 */
        {
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
 * 先对进程组 SIGTERM，等待最多 2 秒，仍不退出则对进程组 SIGKILL，
 * 最后 waitpid 回收组长僵尸（子进程 spawn 时 setpgid(0,0) 自成进程组，
 * 负 pid 信号级联到 runner 派生的全部子进程）。 */
static void agent_kill_and_reap(pid_t *pid_ptr, int *stdin_ptr, int *stdout_ptr)
{
    pid_t pid = *pid_ptr;
    if (pid <= 0)
        return;

    kill(-pid, SIGTERM);
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
        kill(-pid, SIGKILL);
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

/* ==================== 性能监控辅助 ==================== */

/* 单调时钟微秒：POSIX 用 CLOCK_MONOTONIC（不受 NTP/时区跳变影响），
 * Windows 用 GetTickCount64（毫秒精度换算）。用于 spawn/invoke 时延
 * 聚合与慢请求判定。 */
static uint64_t agent_perf_now_us(void)
{
#if AIRY_PLATFORM_POSIX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#else
    return (uint64_t)GetTickCount64() * 1000ull;
#endif
}

/* 全局锁获取：先 trylock，失败计一次锁竞争（原子）再阻塞获取。
 * 10000 并发下锁竞争是首要瓶颈信号，用 trylock 探测可在不改造
 * airy_mtx 的情况下量化等待次数。返回时调用方持有全局锁。 */
static void agent_lock_svc(agent_service_t *svc)
{
    if (airy_mtx_trylock(&svc->lock) != 0) {
        airy_atomic_fetch_add(&svc->m_lock_wait_total, 1);
        airy_mtx_lock(&svc->lock);
    }
}

/* 记录一次耗时到 (累计, 最大值) 聚合器。64 位原子累加 + CAS 更新最大值。 */
static void agent_perf_accumulate(atomic_ullong *us_total, atomic_ullong *us_max,
                                  uint64_t elapsed_us)
{
    atomic_fetch_add_explicit(us_total, elapsed_us, memory_order_relaxed);
    unsigned long long cur = atomic_load_explicit(us_max, memory_order_relaxed);
    while (elapsed_us > cur) {
        if (atomic_compare_exchange_weak_explicit(us_max, &cur, elapsed_us,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }
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
    /* 初始化每个槽位的细粒度锁（并发重构：子进程生命周期操作在
     * entry_lock 下进行，不占用全局锁） */
    for (size_t i = 0; i < max_agents; i++) {
        airy_mtx_init(&svc->agents[i].entry_lock);
        svc->agents[i].status = AGENT_STATUS_FREE;
    }
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
        agent_entry_internal_t *agent = &svc->agents[i];
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

    /* ---- 性能监控：spawn 耗时起点与请求计数 ---- */
    uint64_t perf_t0 = agent_perf_now_us();
    airy_atomic_fetch_add(&svc->m_spawn_total, 1);

    /* ---- 阶段1：全局锁内快速分配槽位（无阻塞 IO，禁止持锁 fork） ---- */
    size_t idx;
    agent_entry_internal_t *agent = NULL;

    agent_lock_svc(svc);
    /* 优先复用空闲槽位（spawn 失败回滚产生），避免 count 因失败膨胀 */
    idx = SIZE_MAX;
    for (size_t i = 0; i < svc->agent_count; i++) {
        if (svc->agents[i].status == AGENT_STATUS_FREE) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX) {
        if (svc->agent_count >= svc->max_agents) {
            airy_mtx_unlock(&svc->lock);
            SVC_LOG_WARN("Agent service full (count=%zu)", svc->agent_count);
            airy_atomic_fetch_add(&svc->m_spawn_fail, 1);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        idx = svc->agent_count++;
    }
    agent = &svc->agents[idx];

    char id_buf[AGENT_ID_LEN];
    agent_generate_agent_id(id_buf, sizeof(id_buf));
    agent->agent_id = AIRY_STRDUP(id_buf);
    agent->spec = AIRY_STRDUP(spec);
    if (!agent->agent_id || !agent->spec) {
        AIRY_FREE(agent->agent_id);
        AIRY_FREE(agent->spec);
        agent->agent_id = NULL;
        agent->spec = NULL;
        agent->status = AGENT_STATUS_FREE;
        airy_mtx_unlock(&svc->lock);
        airy_atomic_fetch_add(&svc->m_spawn_fail, 1);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    agent->status = AGENT_STATUS_SPAWNING;
    agent->spawned_at = (uint64_t)time(NULL);
    airy_mtx_unlock(&svc->lock);

    /* ---- 阶段2：全局锁外 fork + 等待子进程 ready（可并行） ---- */
    pid_t child_pid = -1;
    int child_sin = -1, child_sout = -1;
    int spawn_ok = 1;

#if AIRY_PLATFORM_POSIX
    /* P0-2：真实派生并校验子进程存活，不再静默回退 stub。
     * AIRY_AGENT_NO_SPAWN=1 时跳过 fork（单元测试确定性模式）：视为成功
     * 注册但无子进程，invoke 会返回明确错误而非假成功。 */
    const char *no_spawn_env = getenv("AIRY_AGENT_NO_SPAWN");
    int spawn_disabled = (no_spawn_env && no_spawn_env[0] != '\0' &&
                          strcmp(no_spawn_env, "0") != 0);

    if (spawn_disabled) {
        SVC_LOG_WARN("Agent spawn skipped (AIRY_AGENT_NO_SPAWN): agent_id=%s",
                     agent->agent_id);
    } else if (agent_spawn_child(spec, agent->agent_id,
                                 &child_pid, &child_sin, &child_sout) == 0) {
        /* 等待子进程 ready 确认存活；失败则回收子进程并判定 spawn 失败 */
        char ready_buf[512];
        int ready_rc = agent_read_line_timeout(child_sout, ready_buf,
                                               sizeof(ready_buf),
                                               agent_spawn_ready_timeout_s());
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
        spawn_ok = 0;
    }
#endif

    /* ---- 阶段3：细粒度锁下回填子进程句柄 + 全局锁下注册哈希表 ---- */
    airy_mtx_lock(&agent->entry_lock);

    if (spawn_ok) {
#if AIRY_PLATFORM_POSIX
        if (child_pid > 0) {
            agent->child_pid = child_pid;
            agent->stdin_fd = child_sin;
            agent->stdout_fd = child_sout;
            agent->last_active = (uint64_t)time(NULL);
        }
#endif
        int rc = AIRY_SUCCESS;
        agent_lock_svc(svc);
        if (svc->initialized) {
            rc = agent_ht_insert(&svc->agent_index, agent->agent_id, idx);
        }
        if (rc == AIRY_SUCCESS) {
            agent->status = AGENT_STATUS_RUNNING;
            airy_mtx_unlock(&svc->lock);
            airy_mtx_unlock(&agent->entry_lock);
            *out_agent_id = AIRY_STRDUP(agent->agent_id);
            /* 性能监控：spawn 成功计数 + 耗时聚合 */
            airy_atomic_fetch_add(&svc->m_spawn_ok, 1);
            agent_perf_accumulate(&svc->m_spawn_us_total,
                                  &svc->m_spawn_us_max,
                                  agent_perf_now_us() - perf_t0);
            SVC_LOG_DEBUG("Agent spawn: agent_id=%s, total=%lu",
                          *out_agent_id, (unsigned long)svc->agent_count);
            return AIRY_SUCCESS;
        }
        /* 注册失败：回收子进程并回滚槽位 */
#if AIRY_PLATFORM_POSIX
        if (agent->child_pid > 0) {
            agent_kill_and_reap(&agent->child_pid,
                                &agent->stdin_fd, &agent->stdout_fd);
        }
#endif
        airy_mtx_unlock(&svc->lock);
    }

    /* 失败路径：置空闲并释放资源（agent_id/spec 释放需在全局锁内，
     * 与 list 的并发读保持一致） */
    agent->status = AGENT_STATUS_FREE;
    airy_mtx_unlock(&agent->entry_lock);

    agent_lock_svc(svc);
    AIRY_FREE(agent->agent_id);
    AIRY_FREE(agent->spec);
    agent->agent_id = NULL;
    agent->spec = NULL;
    airy_mtx_unlock(&svc->lock);

    /* 性能监控：spawn 失败计数 */
    airy_atomic_fetch_add(&svc->m_spawn_fail, 1);
    agent_perf_accumulate(&svc->m_spawn_us_total,
                          &svc->m_spawn_us_max,
                          agent_perf_now_us() - perf_t0);

    return spawn_ok ? AIRY_ERR_FAIL : AIRY_ERR_SVC_NOT_READY;
}

int agent_service_terminate(agent_service_t *svc, const char *agent_id)
{
    if (!svc || !svc->initialized || !agent_id)
        return AIRY_ERR_INVALID_PARAM;

    /* 性能监控：terminate 请求计数 */
    airy_atomic_fetch_add(&svc->m_terminate_total, 1);

    agent_lock_svc(svc);

    ssize_t idx = agent_ht_lookup(&svc->agent_index, agent_id);
    if (idx < 0 || (size_t)idx >= svc->agent_count) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    agent_entry_internal_t *agent = &svc->agents[idx];
    airy_mtx_unlock(&svc->lock);

    /* 细粒度锁下终止子进程并置状态，不阻塞其他 agent 操作 */
    airy_mtx_lock(&agent->entry_lock);
#if AIRY_PLATFORM_POSIX
    if (agent->child_pid > 0) {
        agent_kill_and_reap(&agent->child_pid,
                            &agent->stdin_fd,
                            &agent->stdout_fd);
    }
#endif
    /* 仅置终止状态，不回收槽位（与原 syscall_router.c 实现一致） */
    agent->status = AGENT_STATUS_TERMINATED;
    airy_mtx_unlock(&agent->entry_lock);

    SVC_LOG_DEBUG("Agent terminate: agent_id=%s", agent_id);
    return AIRY_SUCCESS;
}

int agent_service_invoke(agent_service_t *svc, const char *agent_id,
                          const char *input, size_t len,
                          airy_cancel_token_t *cancel_token,
                          char **out_output)
{
    if (!svc || !svc->initialized || !agent_id || !out_output)
        return AIRY_ERR_INVALID_PARAM;

    *out_output = NULL;

    /* ---- 性能监控：invoke 耗时起点与请求计数 ---- */
    uint64_t perf_t0 = agent_perf_now_us();
    airy_atomic_fetch_add(&svc->m_invoke_total, 1);

    /* 索引查找仅在全局锁下短暂执行，随即释放 */
    agent_lock_svc(svc);
    ssize_t idx = agent_ht_lookup(&svc->agent_index, agent_id);
    if (idx < 0 || (size_t)idx >= svc->agent_count) {
        airy_mtx_unlock(&svc->lock);
        *out_output = AIRY_STRDUP("{\"error\":\"Agent not found\"}");
        airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
        return AIRY_ERR_NOT_FOUND;
    }
    agent_entry_internal_t *agent = &svc->agents[idx];
    airy_mtx_unlock(&svc->lock);

    /* 子进程通信在细粒度锁下进行：同一 Agent 的调用串行，
     * 不同 Agent 的调用互不阻塞，支持上千 Agent 并行任务 */
    airy_mtx_lock(&agent->entry_lock);

    if (agent->status != AGENT_STATUS_RUNNING) {
        airy_mtx_unlock(&agent->entry_lock);
        *out_output = AIRY_STRDUP("{\"error\":\"Agent not running\"}");
        airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
        return AIRY_ERR_STATE_ERROR;
    }

#if AIRY_PLATFORM_POSIX
    /* Stage5+ 待办4：真实子进程路径 — 通过 stdin/stdout 管道通信。
     * 协议：向子进程 stdin 写一行 JSON 请求，从 stdout 读一行 JSON 响应。
     * child_pid>0 且 stdin_fd>=0 表示有活跃子进程；否则走回退路径。 */
    if (agent->child_pid > 0 && agent->stdin_fd >= 0) {
        int sin_fd = agent->stdin_fd;
        int sout_fd = agent->stdout_fd;

        /* 构建请求 JSON: {"agent_id":..., "input":...} */
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "agent_id", agent_id);
        cJSON_AddStringToObject(req, "input", input ? input : "");
        char *req_str = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);

        if (!req_str) {
            airy_mtx_unlock(&agent->entry_lock);
            *out_output = AIRY_STRDUP("{\"error\":\"failed to build request\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }

        /* 写请求到子进程 stdin（追加 '\n' 作为行分隔） */
        size_t req_len = strlen(req_str);
        char *write_buf = (char *)AIRY_MALLOC(req_len + 2);
        if (!write_buf) {
            AIRY_FREE(req_str);
            airy_mtx_unlock(&agent->entry_lock);
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
            agent_kill_and_reap(&agent->child_pid,
                                &agent->stdin_fd,
                                &agent->stdout_fd);
            goto invoke_fallback;
        }
        /* P0-3：成功向子进程写入请求视为活跃，更新空闲回收基准 */
        agent->last_active = (uint64_t)time(NULL);

        /* 从子进程 stdout 读响应行（带超时 + 取消令牌短轮询，改进1） */
        char *resp_buf = (char *)AIRY_MALLOC(AGENT_RESP_BUF_SIZE);
        if (!resp_buf) {
            airy_mtx_unlock(&agent->entry_lock);
            *out_output = AIRY_STRDUP("{\"error\":\"out of memory\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        int rrc = agent_read_line_timeout_ex(sout_fd, resp_buf,
                                             AGENT_RESP_BUF_SIZE,
                                             agent_invoke_timeout_s(),
                                             cancel_token);
        if (rrc == -2) {
            /* 取消：优雅终止子进程（SIGTERM→2s→SIGKILL 进程组），
             * 以 AbortedOutput 收尾，与超时（fallback 路径）明确区分 */
            AIRY_FREE(resp_buf);
            SVC_LOG_WARN("Agent invoke canceled, terminating child: agent_id=%s",
                         agent_id);
            agent_kill_and_reap(&agent->child_pid,
                                &agent->stdin_fd,
                                &agent->stdout_fd);
            airy_mtx_unlock(&agent->entry_lock);
            *out_output = AIRY_STRDUP(
                "{\"success\":false,\"error\":\"aborted\",\"aborted\":true}");
            airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
            agent_perf_accumulate(&svc->m_invoke_us_total,
                                  &svc->m_invoke_us_max,
                                  agent_perf_now_us() - perf_t0);
            return AIRY_ERR_CANCELED;
        }
        if (rrc < 0) {
            AIRY_FREE(resp_buf);
            SVC_LOG_WARN("Agent invoke read failed, child unusable: agent_id=%s",
                         agent_id);
            agent_kill_and_reap(&agent->child_pid,
                                &agent->stdin_fd,
                                &agent->stdout_fd);
            goto invoke_fallback;
        }
        /* 收到响应同样刷新活跃时间 */
        agent->last_active = (uint64_t)time(NULL);

        /* rrc==1：响应超过 AGENT_RESP_BUF_SIZE 被截断，追加显式标记
         * 防止静默丢数据（调用方可检测 \n...[truncated] 后缀） */
        if (rrc == 1) {
            static const char TRUNC_SUFFIX[] = "\n...[agent response truncated]";
            size_t slen = strlen(resp_buf);
            size_t avail = AGENT_RESP_BUF_SIZE - slen - 1;
            if (avail >= sizeof(TRUNC_SUFFIX) - 1)
                AIRY_MEMCPY(resp_buf + slen, TRUNC_SUFFIX, sizeof(TRUNC_SUFFIX));
            else if (avail > 1)
                AIRY_MEMCPY(resp_buf + slen, TRUNC_SUFFIX, avail - 1);
            resp_buf[AGENT_RESP_BUF_SIZE - 1] = '\0';
            SVC_LOG_WARN("Agent invoke response truncated at %d bytes (agent_id=%s)",
                         (int)AGENT_RESP_BUF_SIZE, agent_id);
        }

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

        airy_mtx_unlock(&agent->entry_lock);
        /* 性能监控：invoke 成功计数 + 耗时聚合 */
        airy_atomic_fetch_add(&svc->m_invoke_ok, 1);
        agent_perf_accumulate(&svc->m_invoke_us_total,
                              &svc->m_invoke_us_max,
                              agent_perf_now_us() - perf_t0);
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

    airy_mtx_unlock(&agent->entry_lock);

    /* 性能监控：invoke 失败计数（fallback 路径：无子进程/通信失败） */
    airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
    agent_perf_accumulate(&svc->m_invoke_us_total,
                          &svc->m_invoke_us_max,
                          agent_perf_now_us() - perf_t0);

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

    agent_lock_svc(svc);

    if (svc->agent_count == 0) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_SUCCESS;
    }

    char **ids = (char **)AIRY_CALLOC(svc->agent_count > 0 ? svc->agent_count : 1,
                                      sizeof(char *));
    if (!ids) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 仅列出 running 状态的 agent（spawning/terminated/free 槽位不对外可见） */
    size_t collected = 0;
    for (size_t i = 0; i < svc->agent_count; i++) {
        if (svc->agents[i].status != AGENT_STATUS_RUNNING)
            continue;
        ids[collected] = AIRY_STRDUP(svc->agents[i].agent_id);
        if (!ids[collected]) {
            for (size_t j = 0; j < collected; j++)
                AIRY_FREE(ids[j]);
            AIRY_FREE(ids);
            airy_mtx_unlock(&svc->lock);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        collected++;
    }

    *out_agent_ids = ids;
    *out_count = collected;

    airy_mtx_unlock(&svc->lock);

    SVC_LOG_DEBUG("Agent list: count=%zu", *out_count);
    return AIRY_SUCCESS;
}

size_t agent_service_count(agent_service_t *svc)
{
    if (!svc || !svc->initialized)
        return 0;
    agent_lock_svc(svc);
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

    /* 全局锁内仅收集空闲槽位索引（快路径），kill 在锁外进行，
     * 避免阻塞其他 agent 操作 */
    size_t *candidates = NULL;
    size_t candidate_count = 0;
    {
        agent_lock_svc(svc);
        if (svc->agent_count > 0) {
            candidates = (size_t *)AIRY_MALLOC(svc->agent_count * sizeof(size_t));
            if (candidates) {
                for (size_t i = 0; i < svc->agent_count; i++) {
                    agent_entry_internal_t *a = &svc->agents[i];
                    if (a->status != AGENT_STATUS_RUNNING || a->child_pid <= 0)
                        continue;
                    if (now <= a->last_active || (now - a->last_active) < max_idle_s)
                        continue;
                    candidates[candidate_count++] = i;
                }
            }
        }
        airy_mtx_unlock(&svc->lock);
    }

#if AIRY_PLATFORM_POSIX
    for (size_t j = 0; j < candidate_count; j++) {
        agent_entry_internal_t *a = &svc->agents[candidates[j]];
        airy_mtx_lock(&a->entry_lock);
        if (a->status == AGENT_STATUS_RUNNING && a->child_pid > 0) {
            SVC_LOG_INFO("Agent idle reclaimed: agent_id=%s, idle=%llus",
                         a->agent_id,
                         (unsigned long long)(now - a->last_active));
            agent_kill_and_reap(&a->child_pid, &a->stdin_fd, &a->stdout_fd);
            a->status = AGENT_STATUS_TERMINATED;
            reaped++;
        }
        airy_mtx_unlock(&a->entry_lock);
    }
#endif
    AIRY_FREE(candidates);

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

int agent_service_get_perf(agent_service_t *svc, agent_perf_stats_t *out)
{
    if (!svc || !out)
        return AIRY_ERR_INVALID_PARAM;

    /* 原子计数器宽松读（监控语义，无需精确一致） */
    out->spawn_total = airy_atomic_load(&svc->m_spawn_total);
    out->spawn_ok = airy_atomic_load(&svc->m_spawn_ok);
    out->spawn_fail = airy_atomic_load(&svc->m_spawn_fail);
    out->invoke_total = airy_atomic_load(&svc->m_invoke_total);
    out->invoke_ok = airy_atomic_load(&svc->m_invoke_ok);
    out->invoke_fail = airy_atomic_load(&svc->m_invoke_fail);
    out->terminate_total = airy_atomic_load(&svc->m_terminate_total);
    out->lock_wait_total = airy_atomic_load(&svc->m_lock_wait_total);
    out->spawn_us_total =
        (unsigned long long)atomic_load_explicit(&svc->m_spawn_us_total,
                                                 memory_order_relaxed);
    out->spawn_us_max =
        (unsigned long long)atomic_load_explicit(&svc->m_spawn_us_max,
                                                 memory_order_relaxed);
    out->invoke_us_total =
        (unsigned long long)atomic_load_explicit(&svc->m_invoke_us_total,
                                                 memory_order_relaxed);
    out->invoke_us_max =
        (unsigned long long)atomic_load_explicit(&svc->m_invoke_us_max,
                                                 memory_order_relaxed);

    /* 峰值并发：扫描 running 槽位计数并 CAS 更新峰值。
     * O(n) 扫描由监控线程周期执行（默认 5s），10000 槽位开销可忽略 */
    size_t running = 0;
    {
        agent_lock_svc(svc);
        for (size_t i = 0; i < svc->agent_count; i++) {
            if (svc->agents[i].status == AGENT_STATUS_RUNNING)
                running++;
        }
        airy_mtx_unlock(&svc->lock);
    }
    int prev_peak = airy_atomic_load(&svc->m_peak_running);
    while ((int)running > prev_peak) {
        if (atomic_compare_exchange_weak_explicit(
                &svc->m_peak_running, &prev_peak, (int)running,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }
    out->peak_running = airy_atomic_load(&svc->m_peak_running);

    return AIRY_SUCCESS;
}
