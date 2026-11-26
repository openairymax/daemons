// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file supervisor_d_internal.h
 * @brief supervisor_d 装配域唯一私有头（gen5 五件套⑤，0.1.18 §12.13 B13）。
 *
 * 域切分：decl（期望态声明解析）/ admit（派生前的内存准入）/ proc
 * （进程域：spawn/reap/退避/假死探测/收摊）/ ctrl（控制域：UDS/TCP +
 * 极简 JSON-RPC）/ net（网络机制域：sock 可达性探测 + Windows TCP 端点
 * 装配权威，域私有头随域下潜 net/sup_net.h）。全户共享契约
 * （sup_ctx/sup_proc、状态机、容量常量与全部跨域函数声明）单点在本
 * 头；net 对外契约经本头消费，域依赖单向 decl/admit/proc/ctrl/net →
 * 契约。装配域（svc.c 生命周期 + main.c 壳）经本头持有全户契约。仅限
 * supervisor_d 编译单元。
 *
 * 期望态 = 画像 launch 声明（$AIRY_HOME/config/profile.env 的
 * AIRYRT_LAUNCH_CORE / AIRYRT_LAUNCH_AUX / AIRYRT_LAUNCH_ARGS_<NAME> /
 * AIRYRT_LAUNCH_BIN_<NAME> / AIRYRT_LAUNCH_EP_<NAME> /
 * AIRYRT_LAUNCH_KEEPENV_<NAME> / AIRYRT_LAUNCH_MEM_<NAME>），
 * 实际态 = 子进程集 + sock 可达性；supervisor_d 周期比对并拉齐：
 * CORE 死而自动复活（指数退避，超限 failed 显式告警），AUX 仅经
 * supervisor.activate 按需拉起。收摊经 supervisor.shutdown 统一收割。
 *
 * V13.5 硬约束：本 target 零项目内库链接（link-whitelist.txt 中允许集为
 * 空，linkgate 构建期 fail-closed）——UDS/TCP、进程原语、JSON 字段提取、
 * 日志全部自持，禁止引入 airy_common/svc_common/atoms/commons。
 * 函数名 ≤ 20 字节（工程规范）。
 */

#ifndef AIRY_SUPERVISOR_D_INTERNAL_H
#define AIRY_SUPERVISOR_D_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* FILE（A-5 输出泵槽位持有落盘句柄） */

#ifdef _WIN32
#include <windows.h>
typedef HANDLE sup_pid_t;
#define SUP_PID_INVALID INVALID_HANDLE_VALUE
#else
#include <pthread.h>
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
#define SUP_ENV_MAX 128

/* ---- 默认参数（env 可覆盖：AIRYRT_SUP_TICK_MS / _BACKOFF_BASE_MS /
 *       _BACKOFF_MAX_MS / _MAX_ATTEMPTS / _LIVENESS_TICKS / _ADMIT_RETRY_MS） ---- */
#define SUP_DEFAULT_TICK_MS 5000
#define SUP_DEFAULT_BACKOFF_BASE_MS 1000
#define SUP_DEFAULT_BACKOFF_MAX_MS 30000
#define SUP_DEFAULT_MAX_ATTEMPTS 5
#define SUP_DEFAULT_LIVENESS_TICKS 3
#define SUP_DEFAULT_READY_MS 15000
#define SUP_DEFAULT_ADMIT_RETRY_MS 30000
#define SUP_TERM_GRACE_MS 5000

/* ---- 角色：CORE = 画像常驻集（死自动复活）；AUX = 按需集（activate 拉起） ---- */
typedef enum sup_role { SUP_ROLE_CORE = 0, SUP_ROLE_AUX = 1 } sup_role_t;

/* ---- 状态机：STOPPED→STARTING→RUNNING；死后 BACKOFF；连败超限 FAILED；
 *      装不下时 WAIT（不计连败，等内存回来再派生） ---- */
typedef enum sup_state {
    SUP_ST_STOPPED = 0,
    SUP_ST_STARTING = 1,
    SUP_ST_RUNNING = 2,
    SUP_ST_BACKOFF = 3,
    SUP_ST_FAILED = 4,
    SUP_ST_WAIT = 5
} sup_state_t;

/* ---- 派生结果：OK 已派生；FAIL 派生失败（计入连败）；DEFER 装不下，等内存 ---- */
typedef enum sup_spawn {
    SUP_SPAWN_OK = 0,
    SUP_SPAWN_FAIL = -1,
    SUP_SPAWN_DEFER = 1
} sup_spawn_t;

typedef struct sup_proc {
    char name[SUP_NAME_MAX];  /* "notify_d" */
    char bin[SUP_PATH_MAX];   /* $AIRY_HOME/bin/notify_d */
    char sock[SUP_PATH_MAX];  /* UDS 路径 / "host:port" / "tcp:<port>"（外部进程回环探活） */
    char args[SUP_ARGS_MAX];  /* 声明注入 argv（如 llm_d --manager <cfg>），空串无参 */
    char keepenv[SUP_ENV_MAX]; /* 保留继承的动态链接器变量白名单，空串=全清洗 */
    sup_role_t role;
    sup_state_t state;
    sup_pid_t pid;
    int fail_count;         /* 连续失败次数（RUNNING 稳定后归零由 next 复活清零） */
    long long next_ms;      /* BACKOFF 最早可重启时刻（monotonic ms） */
    int dead_ticks;         /* sock 不可达连续计数（假死判定） */
    long long ready_ms;     /* 启动宽限（毫秒），0 = 用全局缺省 */
    long long ready_until;  /* 宽限截止（monotonic ms），0 = 无宽限 */
    long long need;         /* 常驻内存声明（字节），0 = 未声明、派生不设门槛 */
    char last_death[128];   /* 最近死因（收割时记录，V13.1 日志判据） */
} sup_proc_t;

#ifndef _WIN32
/* ---- 输出泵（A-5）：子进程 stdout/stderr 经 pipe 汇聚到单一 pump 线程，
 *      首个字节到达时才惰性创建 <name>.out，无输出 daemon 不留 0 字节
 *      日志；AIRY_PREOPEN_OUT=1 回退旧预开行为（排障/兼容用）。槽位按
 *      procs 下标固定，同槽新一代接管前先读空上一代残留。 ---- */
typedef struct sup_pump {
    int rfd[SUP_MAX_DAEMONS];                /* 读端，-1 = 空槽 */
    char out[SUP_MAX_DAEMONS][SUP_PATH_MAX]; /* 惰性 fopen 目标路径 */
    FILE *fp[SUP_MAX_DAEMONS];               /* NULL = 尚未收到字节 */
    int wake[2];                             /* 唤醒管道：写端触发 poll 重扫 */
    pthread_t tid;
    pthread_mutex_t lock;
    int running;
    int stop;
} sup_pump_t;
#endif

typedef struct sup_ctx {
    sup_proc_t procs[SUP_MAX_DAEMONS];
    int count;
    char airy_home[SUP_PATH_MAX];
    char runtime_dir[SUP_PATH_MAX];
    char log_dir[SUP_PATH_MAX];
    char ctrl_ep[SUP_PATH_MAX]; /* POSIX: supervisor.sock 路径；WIN: 127.0.0.1:<AIRY_PORT_SUPERVISOR_D> */
    uint64_t ctrl_dev;          /* 0.1.21 手册 D：ctrl_ep 绑定时的节点身份 */
    uint64_t ctrl_ino;
    int ctrl_ident;             /* 身份记录成功标记；0 时按旧行为删除 */
    long tick_ms;
    long backoff_base_ms;
    long backoff_max_ms;
    int max_attempts;
    int liveness_ticks;
    long long startup_grace_ms; /* 全局启动宽限：探活未就绪不判假死的时长 */
    long admit_retry_ms;        /* WAIT 态重判周期：装不下的进程多久再问一次 */
    int shutdown;               /* 1 = supervisor.shutdown 已受理 */
    int no_respawn;             /* A-9：AIRY_SUPERVISOR_NO_RESPAWN=1 关自动重建 */
    int ready_said;             /* 就绪边沿已记账（防每拍刷屏） */
    int unready_said;           /* 未就绪缺口已记账（同上） */
    long long ready_deadline;   /* 启动宽限截止：此前不就绪不告警 */
#ifndef _WIN32
    sup_pump_t pump;            /* A-5 POSIX 输出泵（Windows 保持预开） */
#endif
} sup_ctx_t;

/* ---- decl.c：期望态声明解析（画像 launch 段 = 期望态 SSoT） ---- */
int sup_decl_load(sup_ctx_t *ctx);
int sup_decl_defaults(sup_ctx_t *ctx);
int sup_proc_find(const sup_ctx_t *ctx, const char *name);
void ensure_dirs(const sup_ctx_t *ctx);

/* ---- admit.c：派生前的内存准入（判定由 airy-rt-cap admit 单点给出） ----
 * 返回 1 = 放行（fit/unknown/未声明），0 = 此刻装不下（refuse，short_out 给出
 * 缺口字节），-1 = 本机无判定（工具缺失或输出不可解析，放行并告警）。 */
long sup_admit_room(const sup_proc_t *p, long long *short_out);

/* ---- svc.c：端到端就绪（依赖 = CORE 子进程端点可达，状态机 RUNNING） ---- */
typedef struct sup_ready {
    int core_total;
    int core_ready;
} sup_ready_t;

const char *sup_state_str(sup_state_t st);
sup_ready_t sup_ready_core(const sup_ctx_t *ctx);
void sup_ready_tick(sup_ctx_t *ctx);

/* ---- proc.c：spawn / reap / 退避 / 假死探测 / 收摊 ---- */
long long sup_now_ms(void);
void sup_proc_reap(sup_ctx_t *ctx);
long long sup_backoff_ms(const sup_ctx_t *ctx, const sup_proc_t *p);
void sup_reconcile(sup_ctx_t *ctx);
long long sup_next_wake_ms(const sup_ctx_t *ctx);
void sup_shutdown_all(sup_ctx_t *ctx);
void sup_log(const char *level, const char *fmt, ...);
void sup_health_tick(sup_ctx_t *ctx, sup_proc_t *p);

#ifndef _WIN32
/* ---- proc.c：派生受管进程前的环境隔离（隔离格；子进程分支调用） ---- */
void sup_env_scrub(const sup_proc_t *p);

/* ---- proc.c：输出泵生命周期与接线（A-5；POSIX only） ---- */
int sup_pump_start(sup_ctx_t *ctx);
void sup_pump_stop(sup_ctx_t *ctx);
int sup_pump_add(sup_ctx_t *ctx, int slot, int rfd, const char *out);
int sup_preopen_env(void);
#endif

/* ---- net 域：端点机制件自持接口（跨平台裁剪 + WSA/TCP 装配权威） ---- */
#include "net/sup_net.h"

/* ---- ctrl.c：控制口（UDS/TCP + 极简 JSON-RPC + CLI 客户端） ---- */
int sup_ctrl_listen(const sup_ctx_t *ctx);
void sup_ctrl_close(int fd);
void sup_ctrl_serve(sup_ctx_t *ctx, int listen_fd, int timeout_ms);
int sup_json_field(const char *json, const char *key, char *out, size_t out_sz);
long long sup_json_num(const char *json, const char *key, long long dflt);
int sup_ctrl_client(sup_ctx_t *ctx, int argc, char **argv);

/* ---- proc.c 提供给 ctrl/main 的拉起决策 ---- */
int sup_activate(sup_ctx_t *ctx, const char *name);

#ifdef _WIN32
/* svc.c 启动时初始化 Job Object（防孤儿：supervisor 死则整组回收） */
int sup_proc_job_init(void);
#endif

/* ---- svc.c：常驻生命周期（prepare/step/clear，自持零依赖） ---- */
int sup_svc_prepare(sup_ctx_t *ctx, int *lfd_out);
int sup_svc_step(sup_ctx_t *ctx, int lfd);
void sup_svc_clear(sup_ctx_t *ctx, int lfd);

/* ---- proc.c：pid 文件防重复启动（唯一启动路径判据） ---- */
int sup_pidfile_write(const sup_ctx_t *ctx);
void sup_pidfile_clear(const sup_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_SUPERVISOR_D_INTERNAL_H */
