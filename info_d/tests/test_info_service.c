// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_info_service.c
 * @brief info_d 信息服务单元测试
 *
 * 通过将守护进程主实现（src/main.c）编译进本测试单元，
 * 从而可访问其 static 实现细节（环形缓冲、L2 方法处理器、分发入口）。
 * 守护进程的 main() 被重命名避免与测试入口冲突。
 */
#define main info_d_impl_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/socket.h>
#include <unistd.h>

static void init_lock(airy_mtx_t *lock)
{
    if (airy_mtx_init(lock) != 0) {
        fprintf(stderr, "FATAL: airy_mtx_init failed\n");
        exit(EXIT_FAILURE);
    }
}

static void make_sockpair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        fprintf(stderr, "FATAL: socketpair failed (errno=%d)\n", errno);
        exit(EXIT_FAILURE);
    }
}

static void write_all(int fd, const char *data)
{
    size_t len = strlen(data);
    if (write(fd, data, len) != (ssize_t)len) {
        fprintf(stderr, "FATAL: write failed (errno=%d)\n", errno);
        exit(EXIT_FAILURE);
    }
}

static void test_history_ring_buffer(void)
{
    printf("  test_history_ring_buffer...\n");

    info_d_service_t svc;
    AIRY_MEMSET(&svc, 0, sizeof(svc));
    init_lock(&svc.lock);

    for (int i = 1; i <= 5; i++) {
        system_info_snapshot_t snap;
        AIRY_MEMSET(&snap, 0, sizeof(snap));
        snap.timestamp = (uint64_t)(1000 + i);
        snap.cpu_usage_pct = (double)i;
        airy_mtx_lock(&svc.lock);
        info_d_history_append(&svc, &snap);
        airy_mtx_unlock(&svc.lock);
    }
    assert(svc.history_count == 5);
    assert(svc.history_head == 5);

    cJSON *arr = info_d_build_history_result(&svc, NULL);
    assert(arr != NULL);
    assert(cJSON_GetArraySize(arr) == 5);
    cJSON *first = cJSON_GetArrayItem(arr, 0);
    cJSON *t0 = cJSON_GetObjectItem(first, "timestamp");
    assert(cJSON_IsNumber(t0) && (uint64_t)t0->valuedouble == 1001);
    cJSON *last = cJSON_GetArrayItem(arr, 4);
    cJSON *t4 = cJSON_GetObjectItem(last, "timestamp");
    assert(cJSON_IsNumber(t4) && (uint64_t)t4->valuedouble == 1005);
    (void)t0;
    (void)t4;
    cJSON_Delete(arr);

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "N", 3);
    arr = info_d_build_history_result(&svc, params);
    assert(arr != NULL);
    assert(cJSON_GetArraySize(arr) == 3);
    cJSON *nfirst = cJSON_GetArrayItem(arr, 0);
    cJSON *nt0 = cJSON_GetObjectItem(nfirst, "timestamp");
    assert(cJSON_IsNumber(nt0) && (uint64_t)nt0->valuedouble == 1003);
    (void)nt0;
    cJSON_Delete(arr);
    cJSON_Delete(params);

    for (int i = 6; i <= 130; i++) {
        system_info_snapshot_t snap;
        AIRY_MEMSET(&snap, 0, sizeof(snap));
        snap.timestamp = (uint64_t)(1000 + i);
        snap.cpu_usage_pct = (double)i;
        airy_mtx_lock(&svc.lock);
        info_d_history_append(&svc, &snap);
        airy_mtx_unlock(&svc.lock);
    }
    assert(svc.history_count == INFO_D_HISTORY_SIZE);
    assert(svc.history_head == (size_t)(130 % INFO_D_HISTORY_SIZE));
    arr = info_d_build_history_result(&svc, NULL);
    assert(arr != NULL);
    assert(cJSON_GetArraySize(arr) == INFO_D_HISTORY_SIZE);
    cJSON *wrap_last = cJSON_GetArrayItem(arr, INFO_D_HISTORY_SIZE - 1);
    cJSON *wt = cJSON_GetObjectItem(wrap_last, "timestamp");
    assert(cJSON_IsNumber(wt) && (uint64_t)wt->valuedouble == 1130);
    (void)wt;
    cJSON_Delete(arr);

    cJSON *params0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(params0, "count", 0);
    arr = info_d_build_history_result(&svc, params0);
    assert(arr != NULL && cJSON_GetArraySize(arr) == 0);
    cJSON_Delete(arr);
    cJSON_Delete(params0);

    airy_mtx_destroy(&svc.lock);
    printf("    PASSED\n");
}

static void test_svc_init(info_d_service_t *svc)
{
    AIRY_MEMSET(svc, 0, sizeof(*svc));
    init_lock(&svc->lock);
    svc->start_time = (uint64_t)time(NULL);
    svc->running = 1;
    svc->collect_running = 1;

    info_d_collect_system_info(&svc->latest_snapshot);
    svc->last_collect_time = svc->latest_snapshot.timestamp;
}

static void test_svc_destroy(info_d_service_t *svc)
{
    airy_mtx_destroy(&svc->lock);
}

static cJSON *read_response(int fd)
{
    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    return cJSON_Parse(buf);
}

static cJSON *check_rpc_ok(const char *label, int fd, int expect_id)
{
    cJSON *resp = read_response(fd);
    assert(resp != NULL);
    cJSON *jsonrpc = cJSON_GetObjectItem(resp, "jsonrpc");
    assert(cJSON_IsString(jsonrpc) && strcmp(jsonrpc->valuestring, "2.0") == 0);
    (void)jsonrpc;
    cJSON *id = cJSON_GetObjectItem(resp, "id");
    assert(cJSON_IsNumber(id) && id->valueint == expect_id);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_IsObject(result) || cJSON_IsArray(result));
    printf("  %s: id=%d result=%s\n", label, id->valueint,
           cJSON_GetArraySize(result) > 0 ? "non-empty" : "empty");
    return resp;
}

static void test_system_method_response(void)
{
    printf("  test_system_method_response...\n");

    info_d_service_t svc;
    test_svc_init(&svc);

    int sv[2];
    make_sockpair(sv);

    handle_system(&svc, NULL, 42, sv[1]);
    close(sv[1]);

    cJSON *resp = check_rpc_ok("info.system", sv[0], 42);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_GetObjectItem(result, "service") != NULL);
    cJSON *hostname = cJSON_GetObjectItem(result, "hostname");
    assert(cJSON_IsString(hostname) && strlen(hostname->valuestring) > 0);
    cJSON *kernel = cJSON_GetObjectItem(result, "kernel_version");
    assert(cJSON_IsString(kernel) && strlen(kernel->valuestring) > 0);
    cJSON *sys = cJSON_GetObjectItem(result, "system");
    assert(cJSON_IsObject(sys));
    cJSON *cores = cJSON_GetObjectItem(sys, "cpu_cores");
    assert(cJSON_IsNumber(cores) && cores->valueint > 0);
    (void)hostname;
    (void)kernel;
    (void)sys;
    (void)cores;
    assert(cJSON_GetObjectItem(sys, "total_memory_kb") != NULL);
    assert(cJSON_GetObjectItem(sys, "free_memory_kb") != NULL);
    assert(cJSON_GetObjectItem(sys, "uptime_sec") != NULL);
    cJSON_Delete(resp);

    close(sv[0]);
    test_svc_destroy(&svc);
    printf("    PASSED\n");
}

static void test_history_method_response(void)
{
    printf("  test_history_method_response...\n");

    info_d_service_t svc;
    test_svc_init(&svc);

    for (int i = 0; i < 3; i++) {
        system_info_snapshot_t snap;
        AIRY_MEMSET(&snap, 0, sizeof(snap));
        snap.timestamp = (uint64_t)(2000 + i);
        airy_mtx_lock(&svc.lock);
        info_d_history_append(&svc, &snap);
        airy_mtx_unlock(&svc.lock);
    }

    int sv[2];
    make_sockpair(sv);
    handle_history(&svc, NULL, 7, sv[1]);
    close(sv[1]);
    cJSON *resp = check_rpc_ok("info.history", sv[0], 7);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_IsArray(result));
    assert(cJSON_GetArraySize(result) == 3);
    cJSON_Delete(resp);
    close(sv[0]);

    make_sockpair(sv);
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "N", 2);
    handle_history(&svc, params, 8, sv[1]);
    close(sv[1]);
    cJSON_Delete(params);
    resp = check_rpc_ok("info.history(N=2)", sv[0], 8);
    result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_IsArray(result));
    assert(cJSON_GetArraySize(result) == 2);
    cJSON *hfirst = cJSON_GetArrayItem(result, 0);
    cJSON *ht0 = cJSON_GetObjectItem(hfirst, "timestamp");
    assert(cJSON_IsNumber(ht0) && (uint64_t)ht0->valuedouble == 2001);
    (void)ht0;
    cJSON_Delete(resp);
    close(sv[0]);

    test_svc_destroy(&svc);
    printf("    PASSED\n");
}

static void test_health_method_response(void)
{
    printf("  test_health_method_response...\n");

    info_d_service_t svc;
    test_svc_init(&svc);

    int sv[2];
    make_sockpair(sv);
    handle_health(&svc, NULL, 9, sv[1]);
    close(sv[1]);

    cJSON *resp = check_rpc_ok("info.health", sv[0], 9);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *status = cJSON_GetObjectItem(result, "status");
    assert(cJSON_IsString(status));
    cJSON *collecting = cJSON_GetObjectItem(result, "collecting");
    assert(cJSON_IsBool(collecting) && cJSON_IsTrue(collecting));
    cJSON *last_ts = cJSON_GetObjectItem(result, "last_collect_time");
    assert(cJSON_IsNumber(last_ts) && last_ts->valuedouble > 0);
    cJSON *uptime = cJSON_GetObjectItem(result, "uptime_s");
    assert(cJSON_IsNumber(uptime));
    cJSON *ts = cJSON_GetObjectItem(result, "timestamp");
    assert(cJSON_IsNumber(ts) && ts->valuedouble > 0);
    (void)status;
    (void)collecting;
    (void)last_ts;
    (void)uptime;
    (void)ts;
    cJSON_Delete(resp);
    close(sv[0]);

    test_svc_destroy(&svc);
    printf("    PASSED\n");
}

static void test_hardware_method_response(void)
{
    printf("  test_hardware_method_response...\n");

    /* SSoT 判据断言：阈值宏与 install.sh/airymaxrt assess_hardware 同口径
     * （minimal：MemTotal < 2.5GiB 或 MemAvailable < 1.5GiB 或 CPU < 3） */
    assert(AIRY_HW_MIN_MEM_TOTAL_KIB == 2560u * 1024u);
    assert(AIRY_HW_MIN_MEM_AVAIL_KIB == 1536u * 1024u);
    assert(AIRY_HW_MIN_CPU_COUNT == 3u);

    /* 直接调用画像 API（真实系统值，仅断言结构合法性与判定自洽） */
    airy_hw_profile_t hw;
    assert(airy_get_hw_profile(&hw) == AIRY_SUCCESS);
    assert(hw.cpu_count > 0);
    assert(hw.mem_total_kib > 0);
    assert(hw.profile == AIRY_HW_PROFILE_MINIMAL ||
           hw.profile == AIRY_HW_PROFILE_FULL);
    /* 判定自洽：full 必须满足全部阈值 */
    if (hw.profile == AIRY_HW_PROFILE_FULL) {
        assert(hw.mem_total_kib >= AIRY_HW_MIN_MEM_TOTAL_KIB);
        assert(hw.mem_avail_kib >= AIRY_HW_MIN_MEM_AVAIL_KIB);
        assert(hw.cpu_count >= AIRY_HW_MIN_CPU_COUNT);
    }
    if (hw.accel_present) {
        assert(hw.accel_count >= 1);
        assert(hw.accel_model[0] != '\0');
    }
    assert(airy_get_hw_profile(NULL) == AIRY_EINVAL);

    info_d_service_t svc;
    test_svc_init(&svc);

    int sv[2];
    make_sockpair(sv);
    handle_hardware(&svc, NULL, 10, sv[1]);
    close(sv[1]);

    cJSON *resp = check_rpc_ok("info.hardware", sv[0], 10);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *cores = cJSON_GetObjectItem(result, "cpu_count");
    assert(cJSON_IsNumber(cores) && cores->valueint > 0);
    cJSON *mem = cJSON_GetObjectItem(result, "mem_total_kib");
    assert(cJSON_IsNumber(mem) && mem->valuedouble > 0);
    cJSON *avail = cJSON_GetObjectItem(result, "mem_avail_kib");
    assert(cJSON_IsNumber(avail));
    cJSON *profile = cJSON_GetObjectItem(result, "profile");
    assert(cJSON_IsString(profile));
    assert(strcmp(profile->valuestring, "full") == 0 ||
           strcmp(profile->valuestring, "minimal") == 0);
    cJSON *accel = cJSON_GetObjectItem(result, "accel_present");
    assert(cJSON_IsBool(accel));
    cJSON *accel_count = cJSON_GetObjectItem(result, "accel_count");
    assert(cJSON_IsNumber(accel_count));
    assert(cJSON_GetObjectItem(result, "accel_model") != NULL);
    (void)cores;
    (void)mem;
    (void)avail;
    (void)profile;
    (void)accel;
    (void)accel_count;
    cJSON_Delete(resp);
    close(sv[0]);

    test_svc_destroy(&svc);
    printf("    PASSED\n");
}

static void test_request_dispatch(void)
{
    printf("  test_request_dispatch...\n");

    info_d_service_t svc;
    test_svc_init(&svc);

    const char *req = "{\"jsonrpc\":\"2.0\",\"method\":\"system\",\"id\":11}";
    int sv[2];
    make_sockpair(sv);
    write_all(sv[0], req);

    info_d_handle_request(&svc, sv[1]);

    cJSON *resp = read_response(sv[0]);
    assert(resp != NULL);
    cJSON *jsonrpc = cJSON_GetObjectItem(resp, "jsonrpc");
    assert(cJSON_IsString(jsonrpc) && strcmp(jsonrpc->valuestring, "2.0") == 0);
    cJSON *id = cJSON_GetObjectItem(resp, "id");
    assert(cJSON_IsNumber(id) && id->valueint == 11);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_IsObject(result));
    (void)jsonrpc;
    (void)id;
    (void)result;
    assert(cJSON_GetObjectItem(result, "system") != NULL);
    cJSON_Delete(resp);
    close(sv[0]);

    const char *unknown_req = "{\"jsonrpc\":\"2.0\",\"method\":\"nope\",\"id\":12}";
    make_sockpair(sv);
    write_all(sv[0], unknown_req);
    info_d_handle_request(&svc, sv[1]);
    resp = read_response(sv[0]);
    assert(resp != NULL);
    assert(cJSON_GetObjectItem(resp, "system") != NULL);
    cJSON_Delete(resp);
    close(sv[0]);

    test_svc_destroy(&svc);
    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Info Service Unit Tests\n");
    printf("=========================================\n");

    test_history_ring_buffer();
    test_system_method_response();
    test_history_method_response();
    test_health_method_response();
    test_hardware_method_response();
    test_request_dispatch();

    printf("\nAll info service tests PASSED\n");
    return 0;
}
