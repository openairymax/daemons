// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file supervisor_d.h
 * @file 运行期 daemon 集群监管者（0.1.18 §12.13 B13）。
 *
 * 期望态 = 画像 launch 声明（$AIRY_HOME/config/profile.env 的
 * AIRYRT_LAUNCH_CORE / AIRYRT_LAUNCH_AUX / AIRYRT_LAUNCH_ARGS_<NAME>），
 * 实际态 = 子进程集 + sock 可达性；supervisor_d 周期比对并拉齐：
 * CORE 死而自动复活（指数退避，超限 failed 显式告警），AUX 仅经
 * supervisor.activate 按需拉起。收摊经 supervisor.shutdown 统一收割。
 *
 * V13.5 硬约束：本 target 零项目内库链接（link-whitelist.txt 中允许集为
 * 空，linkgate 构建期 fail-closed）——UDS/TCP、进程原语、JSON 字段提取、
 * 日志全部自持，禁止引入 airy_common/svc_common/atoms/commons。
 * 函数名 ≤ 20 字节（工程规范）。
 */

#ifndef AIRY_SUPERVISOR_D_H
#define AIRY_SUPERVISOR_D_H

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE sup_pid_t;
#define SUP_PID_INVALID INVALID_HANDLE_VALUE
#else
#include <sys/types.h>
typedef pid_t sup_pid_t;
#define SUP_PID_INVALID (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 容量上限（daemon 总数 0.1.9 整编后为 15，上限留 16） ---- */
#define SUP_MAX_DAEMONS 16
#define SUP_NAME_MAX 32
#define SUP_PATH_MAX 512
#define SUP_ARGS_MAX 512

/* ---- 默认参数（env 可覆盖：AIRYRT_SUP_TICK_MS / _BACKOFF_BASE_MS /
 *       _BACKOFF_MAX_MS / _MAX_ATTEMPTS / _LIVENESS_TICKS） ---- */
#define SUP_DEFAULT_TICK_MS 5000
#define SUP_DEFAULT_BACKOFF_BASE_MS 1000
#define SUP_DEFAULT_BACKOFF_MAX_MS 30000
#define SUP_DEFAULT_MAX_ATTEMPTS 5
#define SUP_DEFAULT_LIVENESS_TICKS 3
#define SUP_TERM_GRACE_MS 5000

/* ---- 角色：CORE = 画像常驻集（死自动复活）；AUX = 按需集（activate 拉起） ---- */
typedef enum sup_role { SUP_ROLE_CORE = 0, SUP_ROLE_AUX = 1 } sup_role_t;

/* ---- 状态机：STOPPED→STARTING→RUNNING；死后 BACKOFF；连败超限 FAILED ---- */
typedef enum sup_state {
    SUP_ST_STOPPED = 0,
    SUP_ST_STARTING = 1,
    SUP_ST_RUNNING = 2,
    SUP_ST_BACKOFF = 3,
    SUP_ST_FAILED = 4
} sup_state_t;

typedef struct sup_proc {
    char name[SUP_NAME_MAX];  /* "notify_d" */
    char bin[SUP_PATH_MAX];   /* $AIRY_HOME/bin/notify_d */
    char sock[SUP_PATH_MAX];  /* POSIX: $AIRY_RUNTIME_DIR/notify.sock；WIN: host:port */
    char args[SUP_ARGS_MAX];  /* 声明注入 argv（如 llm_d --manager <cfg>），空串无参 */
    sup_role_t role;
    sup_state_t state;
    sup_pid_t pid;
    int fail_count;         /* 连续失败次数（RUNNING 稳定后归零由 next 复活清零） */
    long long next_ms;      /* BACKOFF 最早可重启时刻（monotonic ms） */
    int dead_ticks;         /* sock 不可达连续计数（假死判定） */
    char last_death[128];   /* 最近死因（收割时记录，V13.1 日志判据） */
} sup_proc_t;

typedef struct sup_ctx {
    sup_proc_t procs[SUP_MAX_DAEMONS];
    int count;
    char airy_home[SUP_PATH_MAX];
    char runtime_dir[SUP_PATH_MAX];
    char log_dir[SUP_PATH_MAX];
    char ctrl_ep[SUP_PATH_MAX]; /* POSIX: supervisor.sock 路径；WIN: 127.0.0.1:8095 */
    long tick_ms;
    long backoff_base_ms;
    long backoff_max_ms;
    int max_attempts;
    int liveness_ticks;
    int shutdown;               /* 1 = supervisor.shutdown 已受理 */
} sup_ctx_t;

/* ---- decl.c：期望态声明解析（画像 launch 段 = 期望态 SSoT） ---- */
int sup_decl_load(sup_ctx_t *ctx);
int sup_decl_defaults(sup_ctx_t *ctx);
int sup_proc_find(const sup_ctx_t *ctx, const char *name);

/* ---- proc.c：spawn / reap / 退避 / 收摊 ---- */
int sup_proc_spawn(sup_ctx_t *ctx, sup_proc_t *p);
void sup_proc_reap(sup_ctx_t *ctx);
long long sup_backoff_ms(const sup_ctx_t *ctx, const sup_proc_t *p);
void sup_reconcile(sup_ctx_t *ctx);
void sup_shutdown_all(sup_ctx_t *ctx);
long long sup_now_ms(void);
void sup_log(const char *level, const char *fmt, ...);

/* ---- probe.c：sock 可达性（假死层判定） ---- */
int sup_probe_sock(const sup_proc_t *p);
void sup_health_tick(sup_ctx_t *ctx, sup_proc_t *p);

/* ---- ctrl.c：控制口（UDS/TCP + 极简 JSON-RPC） ---- */
int sup_ctrl_listen(const sup_ctx_t *ctx);
void sup_ctrl_close(int fd);
void sup_ctrl_serve(sup_ctx_t *ctx, int listen_fd, int timeout_ms);
int sup_json_field(const char *json, const char *key, char *out, size_t out_sz);

/* ---- proc.c 提供给 ctrl/main 的拉起决策 ---- */
int sup_activate(sup_ctx_t *ctx, const char *name);

#ifdef _WIN32
/* main.c 启动时初始化 Job Object（防孤儿：supervisor 死则整组回收） */
int sup_proc_job_init(void);
#endif

/* ---- main.c 生命周期 ---- */
int sup_pidfile_write(const sup_ctx_t *ctx);
void sup_pidfile_clear(const sup_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_SUPERVISOR_D_H */
