// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_info_rpc.c
 * @brief info 域（monit_d 内建模块）单元测试（0.1.9 M4：info_d → monit_d）。
 *
 * 直接编译 info_rpc.c 真实实现（禁止桩函数），覆盖采集快照、64 深度
 * 环形历史（合成时间戳、环绕覆盖、limit 钳位）与调度器 wire 路径
 * （info_system / info_history / info_health / info_hardware），
 * 并校验硬件画像 SSoT 阈值判据与调度响应自洽性。
 * 测试仅调用 info_rpc_init（不启动采集线程），历史时间序因此确定。
 */

#include "airy_memory.h"
#include "error.h"
#include "info_rpc.h"
#include "jsonrpc_helpers.h"
#include "method_dispatcher.h"
#include "platform_misc.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#define HIST_DEPTH 64

static method_dispatcher_t *g_disp;

static info_snapshot_t make_snap(uint64_t ts)
{
    info_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.timestamp = ts;
    s.cpu_cores = 4;
    s.cpu_usage_pct = 10.0;
    s.total_memory_kb = 4096000;
    s.free_memory_kb = 2048000;
    s.used_memory_kb = 2048000;
    s.memory_usage_pct = 50.0;
    s.disk_total_kb = 100000000;
    s.disk_free_kb = 50000000;
    s.disk_used_kb = 50000000;
    s.disk_usage_pct = 50.0;
    s.uptime_sec = 3600;
    return s;
}

static void test_collect(void)
{
    printf("  test_collect...\n");
    info_snapshot_t snap;
    assert(info_rpc_collect(&snap) == 0);
    assert(snap.cpu_cores > 0);
    assert(snap.total_memory_kb > 0);
    assert(snap.timestamp > 0);
    assert(snap.total_memory_kb >= snap.used_memory_kb);
    assert(snap.memory_usage_pct >= 0.0 && snap.memory_usage_pct <= 100.0);
    assert(snap.disk_usage_pct >= 0.0 && snap.disk_usage_pct <= 100.0);
    assert(info_rpc_collect(NULL) != 0);
    printf("    PASSED\n");
}

static void test_snapshot_json(void)
{
    printf("  test_snapshot_json...\n");
    info_snapshot_t s = make_snap(1234);
    cJSON *o = info_rpc_snapshot_json(&s);
    assert(o != NULL);
    static const char *keys[] = {"timestamp",   "cpu_cores",       "cpu_usage_pct",
                                 "total_memory_kb", "free_memory_kb", "used_memory_kb",
                                 "memory_usage_pct", "disk_total_kb", "disk_free_kb",
                                 "disk_used_kb",  "disk_usage_pct",  "uptime_sec"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        assert(cJSON_GetObjectItem(o, keys[i]) != NULL);
    cJSON *ts = cJSON_GetObjectItem(o, "timestamp");
    assert(cJSON_IsNumber(ts) && ts->valueint == 1234);
    cJSON *cores = cJSON_GetObjectItem(o, "cpu_cores");
    assert(cJSON_IsNumber(cores) && cores->valueint == 4);
    cJSON_Delete(o);
    printf("    PASSED\n");
}

static uint64_t snap_ts(const cJSON *item)
{
    const cJSON *ts = cJSON_GetObjectItem(item, "timestamp");
    assert(cJSON_IsNumber(ts));
    return (uint64_t)ts->valuedouble;
}

static void test_history_ring(void)
{
    printf("  test_history_ring...\n");
    for (int i = 0; i < 130; i++) {
        info_snapshot_t s = make_snap(1001 + (uint64_t)i);
        info_rpc_hist_add(&s);
    }

    cJSON *arr = info_rpc_hist_json(HIST_DEPTH);
    assert(cJSON_GetArraySize(arr) == HIST_DEPTH);
    assert(snap_ts(cJSON_GetArrayItem(arr, 0)) == 1067);
    assert(snap_ts(cJSON_GetArrayItem(arr, HIST_DEPTH - 1)) == 1130);
    cJSON *item = NULL;
    uint64_t prev = 0;
    int idx = 0;
    cJSON_ArrayForEach(item, arr)
    {
        uint64_t ts = snap_ts(item);
        assert(idx == 0 || ts > prev);
        prev = ts;
        idx++;
    }
    cJSON_Delete(arr);

    arr = info_rpc_hist_json(0);
    assert(cJSON_GetArraySize(arr) == 0);
    cJSON_Delete(arr);

    arr = info_rpc_hist_json(-5);
    assert(cJSON_GetArraySize(arr) == 0);
    cJSON_Delete(arr);

    arr = info_rpc_hist_json(3);
    assert(cJSON_GetArraySize(arr) == 3);
    assert(snap_ts(cJSON_GetArrayItem(arr, 0)) == 1128);
    assert(snap_ts(cJSON_GetArrayItem(arr, 2)) == 1130);
    cJSON_Delete(arr);

    arr = info_rpc_hist_json(200); /* 钳位到环深 */
    assert(cJSON_GetArraySize(arr) == HIST_DEPTH);
    cJSON_Delete(arr);
    printf("    PASSED\n");
}

static void test_hw_ssot(void)
{
    printf("  test_hw_ssot...\n");
    /* SSoT 阈值与 install.sh/airymaxrt assess_hardware 同口径 */
    assert(AIRY_HW_MIN_MEM_TOTAL_KIB == 2560u * 1024u);
    assert(AIRY_HW_MIN_MEM_AVAIL_KIB == 1536u * 1024u);
    assert(AIRY_HW_MIN_CPU_COUNT == 3u);

    airy_hw_profile_t hw;
    assert(airy_get_hw_profile(&hw) == AIRY_SUCCESS);
    assert(hw.cpu_count >= 1u);
    assert(hw.mem_total_kib > 0);
    int expect = (hw.mem_total_kib < AIRY_HW_MIN_MEM_TOTAL_KIB ||
                  hw.mem_avail_kib < AIRY_HW_MIN_MEM_AVAIL_KIB ||
                  hw.cpu_count < AIRY_HW_MIN_CPU_COUNT)
                     ? AIRY_HW_PROFILE_MINIMAL
                     : AIRY_HW_PROFILE_FULL;
    assert(hw.profile == expect);
    assert(hw.accel_present == 0 || hw.accel_count >= 1u);
    assert(airy_get_hw_profile(NULL) == AIRY_EINVAL);
    printf("    PASSED\n");
}

#ifndef _WIN32

static char *rpc_roundtrip(const char *method, const char *params_json, int id)
{
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddStringToObject(root, "method", method);
    if (params_json) {
        cJSON *params = cJSON_Parse(params_json);
        assert(params != NULL);
        cJSON_AddItemToObject(root, "params", params);
    }

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    assert(method_dispatcher_dispatch(g_disp, root, jsonrpc_build_error, &fds[0]) == 0);
    cJSON_Delete(root);

    /* 响应可能超过单次 read（64 条历史约 20KB）：循环读直到拼出完整 JSON */
    size_t cap = 32768, len = 0;
    char *buf = AIRY_MALLOC(cap);
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            buf = AIRY_REALLOC(buf, cap);
            assert(buf != NULL);
        }
        ssize_t r = read(fds[1], buf + len, cap - len - 1);
        assert(r > 0);
        len += (size_t)r;
        buf[len] = '\0';
        cJSON *probe = cJSON_Parse(buf);
        if (probe) {
            cJSON_Delete(probe);
            break;
        }
    }
    close(fds[0]);
    close(fds[1]);
    return buf;
}

/* 解析响应并返回 result 节点（root 需调用方自行 cJSON_Delete）。 */
static cJSON *rpc_result(cJSON **root_out, const char *resp, int id)
{
    cJSON *root = cJSON_Parse(resp);
    assert(root != NULL);
    cJSON *jv = cJSON_GetObjectItem(root, "jsonrpc");
    assert(cJSON_IsString(jv) && strcmp(jv->valuestring, "2.0") == 0);
    cJSON *idj = cJSON_GetObjectItem(root, "id");
    assert(cJSON_IsNumber(idj) && idj->valueint == id);
    assert(cJSON_GetObjectItem(root, "error") == NULL);
    cJSON *res = cJSON_GetObjectItem(root, "result");
    assert(res != NULL);
    *root_out = root;
    return res;
}

static void test_rpc_system(void)
{
    printf("  test_rpc_system...\n");
    char *resp = rpc_roundtrip("info_system", NULL, 21);
    cJSON *root = NULL;
    cJSON *result = rpc_result(&root, resp, 21);
    cJSON *svc = cJSON_GetObjectItem(result, "service");
    assert(cJSON_IsString(svc) && strcmp(svc->valuestring, "info") == 0);
    assert(strlen(cJSON_GetObjectItem(result, "platform")->valuestring) > 0);
    assert(strlen(cJSON_GetObjectItem(result, "hostname")->valuestring) > 0);
    assert(strlen(cJSON_GetObjectItem(result, "kernel_version")->valuestring) > 0);
    cJSON *sys = cJSON_GetObjectItem(result, "system");
    assert(cJSON_IsObject(sys));
    assert(cJSON_GetObjectItem(sys, "cpu_cores")->valueint > 0);
    assert(cJSON_GetObjectItem(sys, "timestamp")->valuedouble > 0);
    cJSON_Delete(root);
    AIRY_FREE(resp);
    printf("    PASSED\n");
}

static void test_rpc_history(void)
{
    printf("  test_rpc_history...\n");
    char *resp = rpc_roundtrip("info_history", "{\"N\":2}", 22);
    cJSON *root = NULL;
    cJSON *result = rpc_result(&root, resp, 22);
    assert(cJSON_IsArray(result));
    assert(cJSON_GetArraySize(result) == 2);
    assert(snap_ts(cJSON_GetArrayItem(result, 0)) == 1129);
    assert(snap_ts(cJSON_GetArrayItem(result, 1)) == 1130);
    cJSON_Delete(root);
    AIRY_FREE(resp);

    resp = rpc_roundtrip("info_history", NULL, 23);
    result = rpc_result(&root, resp, 23);
    assert(cJSON_IsArray(result) && cJSON_GetArraySize(result) == HIST_DEPTH);
    cJSON_Delete(root);
    AIRY_FREE(resp);
    printf("    PASSED\n");
}

static void test_rpc_health(void)
{
    printf("  test_rpc_health...\n");
    char *resp = rpc_roundtrip("info_health", NULL, 24);
    cJSON *root = NULL;
    cJSON *result = rpc_result(&root, resp, 24);
    cJSON *status = cJSON_GetObjectItem(result, "status");
    assert(cJSON_IsString(status));
    /* 测试未启动采集线程：collecting=0 → 必然 degraded */
    assert(strcmp(status->valuestring, "degraded") == 0);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "collecting")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "running")));
    assert(cJSON_GetObjectItem(result, "last_collect_time")->valuedouble > 0);
    assert(cJSON_GetObjectItem(result, "timestamp")->valuedouble > 0);
    cJSON_Delete(root);
    AIRY_FREE(resp);
    printf("    PASSED\n");
}

static void test_rpc_hardware(void)
{
    printf("  test_rpc_hardware...\n");
    char *resp = rpc_roundtrip("info_hardware", NULL, 25);
    cJSON *root = NULL;
    cJSON *result = rpc_result(&root, resp, 25);
    cJSON *cpu = cJSON_GetObjectItem(result, "cpu_count");
    cJSON *total = cJSON_GetObjectItem(result, "mem_total_kib");
    cJSON *avail = cJSON_GetObjectItem(result, "mem_avail_kib");
    cJSON *profile = cJSON_GetObjectItem(result, "profile");
    assert(cJSON_IsNumber(cpu) && cpu->valuedouble >= 1);
    assert(cJSON_IsNumber(total) && total->valuedouble > 0);
    assert(cJSON_IsNumber(avail));
    assert(cJSON_IsString(profile));
    /* 响应自身字段即可复算画像：SSoT 判据自洽 */
    int minimal = (total->valuedouble < (double)AIRY_HW_MIN_MEM_TOTAL_KIB ||
                   avail->valuedouble < (double)AIRY_HW_MIN_MEM_AVAIL_KIB ||
                   cpu->valuedouble < (double)AIRY_HW_MIN_CPU_COUNT);
    assert(strcmp(profile->valuestring, minimal ? "minimal" : "full") == 0);
    assert(cJSON_IsBool(cJSON_GetObjectItem(result, "accel_present")));
    assert(cJSON_IsNumber(cJSON_GetObjectItem(result, "accel_count")));
    cJSON_Delete(root);
    AIRY_FREE(resp);
    printf("    PASSED\n");
}
#endif /* !_WIN32 */

int main(void)
{
    printf("=========================================\n");
    printf("  Monit info_rpc Module Tests\n");
    printf("=========================================\n");
    fflush(stdout);

    airy_log_init(NULL);
    assert(info_rpc_init() == 0);
    assert(info_rpc_init() == 0); /* 幂等 */
#ifndef _WIN32
    g_disp = method_dispatcher_create(16);
    assert(g_disp != NULL);
    info_rpc_register(g_disp);
#endif

    test_collect();
    test_snapshot_json();
    test_history_ring();
    test_hw_ssot();
#ifndef _WIN32
    test_rpc_system();
    test_rpc_history();
    test_rpc_health();
    test_rpc_hardware();
    method_dispatcher_destroy(g_disp);
#endif

    info_rpc_cleanup();
    info_rpc_cleanup(); /* 幂等 */
    printf("\nAll info_rpc tests PASSED\n");
    return 0;
}
