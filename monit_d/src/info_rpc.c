// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file info_rpc.c
 * @brief 系统信息域 RPC 服务（0.1.9 M4：info_d → monit_d 整编）。
 *
 * info_d（系统信息采集域）并入 monit_d（监控域，采集与可观测同域）：
 * info.system/history/health/hardware 以 info_* 前缀登记到 monit 调度器；
 * get_stats / health_check 走 monit 原生实现。5s 周期采集线程与 64 深度
 * 环形历史随迁，响应 service 字段改命名空间名 "info"。
 * 原独立 server_fd、event_loop accept 路径与 raw 状态 JSON 兜底随
 * info_d 消亡（消费方仅经 gateway JSON-RPC 转发访问）。
 */

#include "airy_memory.h"
#include "daemon_main.h"
#include "error.h"
#include "info_rpc.h"
#include "jsonrpc_helpers.h"
#include "method_dispatcher.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/time.h>
#include <mach/mach.h>
#endif
#endif

#define INFO_RPC_COLLECT_INTERVAL_SEC 5
#define INFO_RPC_HIST_SIZE 64

static airy_mtx_t g_info_lock;
static airy_thread_t g_info_collect_thread;
static atomic_int g_info_collect_running;
static uint64_t g_info_start_time;
static uint64_t g_info_last_collect;
static info_snapshot_t g_info_latest;
static info_snapshot_t g_info_hist[INFO_RPC_HIST_SIZE];
static size_t g_info_hist_count;
static size_t g_info_hist_head;
static int g_info_ready;
static int g_info_thread_started;

int info_rpc_collect(info_snapshot_t *snap)
{
    if (!snap)
        return AIRY_EINVAL;
    __builtin_memset(snap, 0, sizeof(*snap));
    snap->timestamp = (uint64_t)time(NULL);

#ifdef _WIN32
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    snap->cpu_cores = (int)sys_info.dwNumberOfProcessors;

    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        snap->total_memory_kb = (uint64_t)(mem_status.ullTotalPhys / 1024);
        snap->free_memory_kb = (uint64_t)(mem_status.ullAvailPhys / 1024);
        snap->used_memory_kb = snap->total_memory_kb - snap->free_memory_kb;
        if (snap->total_memory_kb > 0)
            snap->memory_usage_pct =
                (double)snap->used_memory_kb / (double)snap->total_memory_kb * 100.0;
    }

    ULARGE_INTEGER total_bytes, free_bytes, avail_bytes;
    if (GetDiskFreeSpaceExW(L"C:\\", &avail_bytes, &total_bytes, &free_bytes)) {
        snap->disk_total_kb = (uint64_t)(total_bytes.QuadPart / 1024);
        snap->disk_free_kb = (uint64_t)(free_bytes.QuadPart / 1024);
        snap->disk_used_kb = snap->disk_total_kb - snap->disk_free_kb;
        if (snap->disk_total_kb > 0)
            snap->disk_usage_pct = (double)snap->disk_used_kb / (double)snap->disk_total_kb * 100.0;
    }
#else
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    snap->cpu_cores = (int)(nproc > 0 ? nproc : 1);

#ifdef __APPLE__
    /* macOS: sysctl 取物理内存，host_statistics 取空闲页，
     * KERN_BOOTTIME 算 uptime（macOS 无 sysinfo(2)）。 */
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctl(mib, 2, &memsize, &len, NULL, 0) == 0 && memsize > 0)
        snap->total_memory_kb = (uint64_t)(memsize / 1024);

    vm_size_t page_size = 0;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vm_stat;
    mach_port_t host = mach_host_self();
    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm_stat, &cnt) == KERN_SUCCESS &&
        host_page_size(host, &page_size) == KERN_SUCCESS) {
        uint64_t free_pages = vm_stat.free_count + vm_stat.inactive_count;
        snap->free_memory_kb = free_pages * page_size / 1024;
    }
    mach_port_deallocate(mach_task_self(), host);
    snap->used_memory_kb = snap->total_memory_kb - snap->free_memory_kb;
    if (snap->total_memory_kb > 0)
        snap->memory_usage_pct =
            (double)snap->used_memory_kb / (double)snap->total_memory_kb * 100.0;

    struct timeval boottime, now;
    len = sizeof(boottime);
    mib[0] = CTL_KERN;
    mib[1] = KERN_BOOTTIME;
    if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0 && gettimeofday(&now, NULL) == 0 &&
        now.tv_sec >= boottime.tv_sec)
        snap->uptime_sec = (uint64_t)(now.tv_sec - boottime.tv_sec);
#else
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        snap->total_memory_kb = (uint64_t)(si.totalram / 1024);
        snap->free_memory_kb = (uint64_t)(si.freeram / 1024);
        snap->used_memory_kb = snap->total_memory_kb - snap->free_memory_kb;
        if (snap->total_memory_kb > 0)
            snap->memory_usage_pct =
                (double)snap->used_memory_kb / (double)snap->total_memory_kb * 100.0;
        snap->uptime_sec = (uint64_t)si.uptime;
    }
#endif

    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        snap->disk_total_kb = (uint64_t)(vfs.f_blocks * vfs.f_frsize / 1024);
        snap->disk_free_kb = (uint64_t)(vfs.f_bfree * vfs.f_frsize / 1024);
        snap->disk_used_kb = snap->disk_total_kb - snap->disk_free_kb;
        if (snap->disk_total_kb > 0)
            snap->disk_usage_pct = (double)snap->disk_used_kb / (double)snap->disk_total_kb * 100.0;
    }

    /* cpu_usage_pct 需两次采样差值，单快照口径固定 0.0（保持既有契约）。 */
    snap->cpu_usage_pct = 0.0;
#endif

    return 0;
}

void info_rpc_hist_add(const info_snapshot_t *snap)
{
    if (!snap)
        return;
    airy_mtx_lock(&g_info_lock);
    g_info_hist[g_info_hist_head] = *snap;
    g_info_hist_head = (g_info_hist_head + 1) % INFO_RPC_HIST_SIZE;
    if (g_info_hist_count < INFO_RPC_HIST_SIZE)
        g_info_hist_count++;
    airy_mtx_unlock(&g_info_lock);
}

cJSON *info_rpc_hist_json(int limit)
{
    if (limit < 0)
        limit = 0;
    if (limit > INFO_RPC_HIST_SIZE)
        limit = INFO_RPC_HIST_SIZE;

    cJSON *arr = cJSON_CreateArray();
    airy_mtx_lock(&g_info_lock);
    size_t take = g_info_hist_count < (size_t)limit ? g_info_hist_count : (size_t)limit;
    for (size_t i = 0; i < take; i++) {
        size_t idx = (g_info_hist_head + INFO_RPC_HIST_SIZE - take + i) % INFO_RPC_HIST_SIZE;
        cJSON_AddItemToArray(arr, info_rpc_snap_json(&g_info_hist[idx]));
    }
    airy_mtx_unlock(&g_info_lock);
    return arr;
}

#ifdef _WIN32
static DWORD WINAPI info_collect_loop(LPVOID arg)
{
#else
static void *info_collect_loop(void *arg)
{
#endif
    (void)arg;
    while (atomic_load_explicit(&g_info_collect_running, memory_order_relaxed)) {
        info_snapshot_t snap;
        info_rpc_collect(&snap);
        info_rpc_hist_add(&snap);

        airy_mtx_lock(&g_info_lock);
        g_info_latest = snap;
        g_info_last_collect = snap.timestamp;
        airy_mtx_unlock(&g_info_lock);

        for (int i = 0; i < INFO_RPC_COLLECT_INTERVAL_SEC &&
                    atomic_load_explicit(&g_info_collect_running, memory_order_relaxed);
             i++) {
#ifdef _WIN32
            Sleep(1000);
#else
            sleep(1);
#endif
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ── 响应构建 ──────────────────────────────────────────────────────── */

cJSON *info_rpc_snap_json(const info_snapshot_t *snap)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "timestamp", (double)snap->timestamp);
    cJSON_AddNumberToObject(item, "cpu_cores", snap->cpu_cores);
    cJSON_AddNumberToObject(item, "cpu_usage_pct", snap->cpu_usage_pct);
    cJSON_AddNumberToObject(item, "total_memory_kb", (double)snap->total_memory_kb);
    cJSON_AddNumberToObject(item, "free_memory_kb", (double)snap->free_memory_kb);
    cJSON_AddNumberToObject(item, "used_memory_kb", (double)snap->used_memory_kb);
    cJSON_AddNumberToObject(item, "memory_usage_pct", snap->memory_usage_pct);
    cJSON_AddNumberToObject(item, "disk_total_kb", (double)snap->disk_total_kb);
    cJSON_AddNumberToObject(item, "disk_free_kb", (double)snap->disk_free_kb);
    cJSON_AddNumberToObject(item, "disk_used_kb", (double)snap->disk_used_kb);
    cJSON_AddNumberToObject(item, "disk_usage_pct", snap->disk_usage_pct);
    cJSON_AddNumberToObject(item, "uptime_sec", (double)snap->uptime_sec);
    return item;
}

static cJSON *build_sys_json(void)
{
    airy_mtx_lock(&g_info_lock);
    info_snapshot_t snap = g_info_latest;
    airy_mtx_unlock(&g_info_lock);

    const char *platform_name = "Linux";
    const char *hostname = "localhost";
    const char *kernel_release = "unknown";
#ifndef _WIN32
    struct utsname uts;
    if (uname(&uts) == 0) {
        platform_name = uts.sysname;
        hostname = uts.nodename;
        kernel_release = uts.release;
    }
#endif

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "info");
    cJSON_AddStringToObject(result, "platform", platform_name);
    cJSON_AddStringToObject(result, "hostname", hostname);
    cJSON_AddStringToObject(result, "kernel_version", kernel_release);
    cJSON_AddItemToObject(result, "system", info_rpc_snap_json(&snap));
    return result;
}

static cJSON *build_health_json(void)
{
    airy_mtx_lock(&g_info_lock);
    int collecting = atomic_load_explicit(&g_info_collect_running, memory_order_relaxed);
    uint64_t last_collect = g_info_last_collect;
    airy_mtx_unlock(&g_info_lock);

    uint64_t now = (uint64_t)time(NULL);
    uint64_t uptime_s = now > g_info_start_time ? now - g_info_start_time : 0;
    uint64_t staleness = now > last_collect ? now - last_collect : 0;
    const char *status =
        (collecting && staleness <= (uint64_t)(INFO_RPC_COLLECT_INTERVAL_SEC * 3)) ? "ok" :
                                                                                    "degraded";

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", status);
    cJSON_AddStringToObject(result, "service", "info");
    cJSON_AddBoolToObject(result, "collecting", collecting ? 1 : 0);
    cJSON_AddBoolToObject(result, "running", g_info_ready ? 1 : 0);
    cJSON_AddNumberToObject(result, "last_collect_time", (double)last_collect);
    cJSON_AddNumberToObject(result, "staleness_sec", (double)staleness);
    cJSON_AddNumberToObject(result, "uptime_s", (double)uptime_s);
    cJSON_AddNumberToObject(result, "timestamp", (double)now);
    return result;
}

/* 硬件画像查询（0.1.6 P1-3/d）：CPU/内存/画像(minimal|full)/加速器聚合。
 * 与 install.sh/airymaxrt assess_hardware 同口径（C 侧 SSoT 判据）。 */
static cJSON *build_hw_json(void)
{
    cJSON *result = cJSON_CreateObject();
    airy_hw_profile_t hw;
    if (airy_get_hw_profile(&hw) != AIRY_SUCCESS) {
        cJSON_AddStringToObject(result, "status", "unavailable");
        return result;
    }
    cJSON_AddNumberToObject(result, "cpu_count", hw.cpu_count);
    cJSON_AddNumberToObject(result, "mem_total_kib", (double)hw.mem_total_kib);
    cJSON_AddNumberToObject(result, "mem_avail_kib", (double)hw.mem_avail_kib);
    cJSON_AddStringToObject(result, "profile",
                            hw.profile == AIRY_HW_PROFILE_FULL ? "full" : "minimal");
    cJSON_AddBoolToObject(result, "accel_present", hw.accel_present);
    cJSON_AddNumberToObject(result, "accel_count", hw.accel_count);
    cJSON_AddStringToObject(result, "accel_model", hw.accel_model[0] ? hw.accel_model : "");
    return result;
}

/* ── RPC 处理器 ───────────────────────────────────────────────────── */

static void info_h_system(cJSON *params, int id, airy_sock_t client_fd)
{
    (void)params;
    JSONRPC_SEND_SUCCESS(client_fd, build_sys_json(), id);
}

static void info_h_history(cJSON *params, int id, airy_sock_t client_fd)
{
    int limit = INFO_RPC_HIST_SIZE;
    if (cJSON_IsObject(params)) {
        cJSON *n = cJSON_GetObjectItem(params, "N");
        if (!cJSON_IsNumber(n))
            n = cJSON_GetObjectItem(params, "n");
        if (!cJSON_IsNumber(n))
            n = cJSON_GetObjectItem(params, "count");
        if (!cJSON_IsNumber(n))
            n = cJSON_GetObjectItem(params, "limit");
        if (cJSON_IsNumber(n))
            limit = n->valueint;
    } else if (cJSON_IsArray(params) && cJSON_GetArraySize(params) > 0) {
        cJSON *n = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsNumber(n))
            limit = n->valueint;
    }
    JSONRPC_SEND_SUCCESS(client_fd, info_rpc_hist_json(limit), id);
}

static void info_h_health(cJSON *params, int id, airy_sock_t client_fd)
{
    (void)params;
    JSONRPC_SEND_SUCCESS(client_fd, build_health_json(), id);
}

static void info_h_hardware(cJSON *params, int id, airy_sock_t client_fd)
{
    (void)params;
    JSONRPC_SEND_SUCCESS(client_fd, build_hw_json(), id);
}

#define INFO_DECLARE(name, fn)                                                                  \
    static void info_m_##name(cJSON *params, int id, void *user_data)                            \
    {                                                                                           \
        fn(params, id, *(airy_sock_t *)user_data);                                                \
    }

INFO_DECLARE(system, info_h_system)
INFO_DECLARE(history, info_h_history)
INFO_DECLARE(health, info_h_health)
INFO_DECLARE(hardware, info_h_hardware)

int info_rpc_init(void)
{
    if (g_info_ready)
        return 0;

    __builtin_memset(g_info_hist, 0, sizeof(g_info_hist));
    g_info_hist_count = 0;
    g_info_hist_head = 0;
    g_info_start_time = (uint64_t)time(NULL);
    airy_mtx_init(&g_info_lock);
    g_info_ready = 1;

    info_rpc_collect(&g_info_latest);
    g_info_last_collect = g_info_latest.timestamp;

    SVC_LOG_INFO("info_rpc: init complete (cpu_cores=%d, collect_interval=%ds)",
                 g_info_latest.cpu_cores, INFO_RPC_COLLECT_INTERVAL_SEC);
    return 0;
}

/* 周期采集线程与状态初始化分离：main 启动线程；单元测试仅初始化状态。 */
int info_rpc_start(void)
{
    if (!g_info_ready)
        return AIRY_EINVAL;
    if (g_info_thread_started)
        return 0;

    atomic_store_explicit(&g_info_collect_running, 1, memory_order_relaxed);
    if (airy_thread_create(&g_info_collect_thread, info_collect_loop, NULL) != 0) {
        SVC_LOG_ERROR("info_rpc: failed to start collector thread");
        atomic_store_explicit(&g_info_collect_running, 0, memory_order_relaxed);
        return AIRY_ENOMEM;
    }
    g_info_thread_started = 1;
    return 0;
}

void info_rpc_cleanup(void)
{
    if (!g_info_ready)
        return;

    atomic_store_explicit(&g_info_collect_running, 0, memory_order_relaxed);
    if (g_info_thread_started) {
        airy_thread_join(g_info_collect_thread, NULL);
        g_info_thread_started = 0;
    }
    airy_mtx_destroy(&g_info_lock);
    g_info_ready = 0;
    SVC_LOG_INFO("info_rpc: cleaned up");
}

void info_rpc_register(void *disp)
{
    method_dispatcher_t *d = (method_dispatcher_t *)disp;
    if (!d)
        return;
    /* info_* 前缀登记：与 monit 既有方法名区分（如 health_check 走 monit
     * 原生）；get_stats/health_check 复用 monit 原生方法，不在此登记。 */
    method_dispatcher_register(d, "info_system", info_m_system, NULL);
    method_dispatcher_register(d, "info_history", info_m_history, NULL);
    method_dispatcher_register(d, "info_health", info_m_health, NULL);
    method_dispatcher_register(d, "info_hardware", info_m_hardware, NULL);
    SVC_LOG_INFO("info_rpc: registered info.* methods on monit_d dispatcher "
                 "(info_system/history/health/hardware)");
}
