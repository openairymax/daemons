// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_l2_bridge.c
 * @brief 8.3.2: daemon L2 envelope bridge unit tests
 *
 * 测试场景：
 *   1. encode 参数校验：NULL buf / NULL payload+len>0 / 超限 / 容量不足
 *   2. envelope 字段级 roundtrip：magic/opcode/flags/trace/timestamp/src/
 *      dst/badge/payload_len/crc32/reserved 全零 + decode 零拷贝视图
 *   3. 空 payload 边界：encode(NULL,0) -> crc=0，decode 视图 NULL/0
 *   4. decode 损坏注入：短 buf / 坏 magic / 保留 flags 位 / 非零 reserved
 *      字节 / 严格长度双向偏差 / payload_len 超限 / CRC 双向篡改 / NULL 参数
 *      （篡改一律 AIRY_MEMCPY 局部 struct 往返，禁止对齐 UB 强转）
 *   5. start 参数校验：NULL 名 / 空名 / NULL dispatch / stop(NULL) 安全
 *   6. E2E echo：call 全链（trace_id 贯穿、src_task 回显、payload 大写变换）
 *   7. E2E junk drop：非 envelope 输入 -> AIRY_ERR_CANCELED 且不触达 dispatch
 *   8. E2E dispatch 失败：非零返回 -> call 失败
 *   9. 8.3.3 channel 派生：daemon_l2_channel_for_socket（.sock 命名判定 /
 *      transport switch fail-closed / "<ns>.rpc" 派生 / 容量与 ns 长度守卫）
 *  10. 8.3.3 rpc_call E2E：result 解包 / params 嵌入 / 业务错误与空载荷
 *      折叠 GENERIC_FAIL / 未挂载 channel NOT_FOUND 透传
 *  11. 8.3.3 rpc_call_resp E2E：完整 JSON-RPC 响应原样透传（含 error 对象）
 *
 * 客户端直接使用 corekern L1 API（airy_ipc_connect/call）模拟对端；
 * corekern 头仅在本测试 TU 使用，不违反 svc_common 的宏隔离边界。
 */

#include "daemon_l1_server.h"

#include "ipc.h"

#include <airymax/ipc.h>       /* [SC] SSoT: AIRY_IPC_MAGIC */
#include <airymax/task_desc.h> /* [SC] CRC-32 reference for field assertions */

#include <cjson/cJSON.h> /* 8.3.3: rpc_dispatch mock 解析 JSON-RPC 请求 */

#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define l2t_getpid() ((uint64_t)_getpid())
#else
#include <unistd.h>
#define l2t_getpid() ((uint64_t)getpid())
#endif

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name)           \
    do {                           \
        printf("  %s...\n", name); \
    } while (0)

#define TEST_ASSERT(cond, msg)                                 \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("    FAIL: %s (line %d)\n", msg, __LINE__); \
            g_tests_failed++;                                  \
            return;                                            \
        }                                                      \
    } while (0)

#define TEST_END()              \
    do {                        \
        printf("    PASSED\n"); \
        g_tests_passed++;       \
    } while (0)

/* ---- encode 参数校验 ---- */

static void test_encode_param_validation(void)
{
    TEST_BEGIN("test_encode_param_validation");

    uint8_t buf[AIRY_IPC_HDR_SIZE + 4];
    TEST_ASSERT(daemon_l2_envelope_encode(1, 2, 3, "x", 1, NULL, sizeof(buf)) == AIRY_EINVAL,
                "NULL out_buf rejected");
    TEST_ASSERT(daemon_l2_envelope_encode(1, 2, 3, NULL, 1, buf, sizeof(buf)) == AIRY_EINVAL,
                "NULL payload with len>0 rejected");
    TEST_ASSERT(daemon_l2_envelope_encode(1, 2, 3, NULL, 0, buf, sizeof(buf)) == 0,
                "NULL payload with len=0 is the empty-payload form");
    TEST_ASSERT(daemon_l2_envelope_encode(1, 2, 3, "x", DAEMON_L2_MAX_PAYLOAD + 1, buf,
                                          sizeof(buf)) == AIRY_EMSGSIZE,
                "payload above DAEMON_L2_MAX_PAYLOAD rejected");
    TEST_ASSERT(daemon_l2_envelope_encode(1, 2, 3, "x", 1, buf, AIRY_IPC_HDR_SIZE) ==
                    AIRY_EMSGSIZE,
                "undersized out_buf rejected");

    TEST_END();
}

/* ---- envelope 字段级 roundtrip ---- */

static void test_envelope_roundtrip_fields(void)
{
    TEST_BEGIN("test_envelope_roundtrip_fields");

    static const char payload[] = "l2-roundtrip";
    uint8_t buf[AIRY_IPC_HDR_SIZE + sizeof(payload)];
    TEST_ASSERT(daemon_l2_envelope_encode(0x1122334455667788ULL, 7, 9, payload,
                                          sizeof(payload), buf, sizeof(buf)) == 0,
                "encode succeeded");

    struct airy_ipc_msg_hdr hdr;
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    TEST_ASSERT(hdr.magic == AIRY_IPC_MAGIC, "magic");
    TEST_ASSERT(hdr.opcode == AIRY_IPC_OP_SEND, "opcode fixed to SEND");
    TEST_ASSERT(hdr.flags == 0, "flags zero (C-S10 clean)");
    TEST_ASSERT(hdr.trace_id == 0x1122334455667788ULL, "trace_id");
    TEST_ASSERT(hdr.timestamp_ns != 0, "monotonic timestamp filled (8.2.3 SSoT)");
    TEST_ASSERT(hdr.src_task == 7, "src_task");
    TEST_ASSERT(hdr.dst_task == 9, "dst_task");
    TEST_ASSERT(hdr.capability_badge == 0, "badge zero (capability folding unused)");
    TEST_ASSERT(hdr.payload_len == sizeof(payload), "payload_len");
    TEST_ASSERT(hdr.crc32 == airy_task_desc_crc32(payload, sizeof(payload)), "crc32");
    int reserved_nonzero = 0;
    for (size_t i = 0; i < sizeof(hdr.reserved); i++) {
        reserved_nonzero |= hdr.reserved[i];
    }
    TEST_ASSERT(reserved_nonzero == 0, "reserved zeroed by encode");

    const void *view = NULL;
    size_t view_len = 0;
    uint64_t trace = 0;
    uint64_t src = 0;
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    0,
                "decode succeeded");
    TEST_ASSERT(view && view_len == sizeof(payload) && memcmp(view, payload, view_len) == 0,
                "payload view is zero-copy over the envelope");
    TEST_ASSERT(trace == 0x1122334455667788ULL && src == 7, "trace/src round-trip");

    TEST_END();
}

/* ---- 空 payload 边界 ---- */

static void test_envelope_empty_payload(void)
{
    TEST_BEGIN("test_envelope_empty_payload");

    uint8_t buf[AIRY_IPC_HDR_SIZE];
    TEST_ASSERT(daemon_l2_envelope_encode(42, 1, 2, NULL, 0, buf, sizeof(buf)) == 0,
                "empty payload encoded");

    struct airy_ipc_msg_hdr hdr;
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    TEST_ASSERT(hdr.payload_len == 0 && hdr.crc32 == 0, "len=0 crc=0 (no CRC over empty)");

    const void *view = NULL;
    size_t view_len = 1;
    uint64_t trace = 0;
    uint64_t src = 0;
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    0,
                "empty envelope decoded");
    TEST_ASSERT(view == NULL && view_len == 0, "empty payload view is NULL/0");

    TEST_END();
}

/* ---- decode 损坏注入 ---- */

static void test_decode_corruption(void)
{
    TEST_BEGIN("test_decode_corruption");

    static const char payload[] = "corrupt-me";
    const size_t payload_len = sizeof(payload);
    uint8_t buf[AIRY_IPC_HDR_SIZE + payload_len];
    struct airy_ipc_msg_hdr hdr;
    const void *view = NULL;
    size_t view_len = 0;
    uint64_t trace = 0;
    uint64_t src = 0;

    TEST_ASSERT(daemon_l2_envelope_encode(1, 2, 3, payload, payload_len, buf, sizeof(buf)) ==
                    0,
                "baseline envelope encoded");

    /* 短 buf */
    TEST_ASSERT(daemon_l2_envelope_decode(buf, AIRY_IPC_HDR_SIZE - 1, &view, &view_len,
                                          &trace, &src) == AIRY_EMSGSIZE,
                "buf shorter than header rejected");

    /* 坏 magic */
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    hdr.magic = 0xDEADBEEFu;
    AIRY_MEMCPY(buf, &hdr, sizeof(hdr));
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    AIRY_ERR_PROTOCOL,
                "wrong magic -> PROTOCOL (drop, not reply)");

    /* 保留 flags 位（C-S10） */
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    hdr.magic = AIRY_IPC_MAGIC;
    hdr.flags = 0x0020u; /* 第一个保留位 */
    AIRY_MEMCPY(buf, &hdr, sizeof(hdr));
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    AIRY_ERR_PROTOCOL,
                "reserved flag bit -> PROTOCOL");

    /* 非零 reserved 字节 */
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    hdr.flags = 0;
    hdr.reserved[71] = 1;
    AIRY_MEMCPY(buf, &hdr, sizeof(hdr));
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    AIRY_ERR_PROTOCOL,
                "non-zero reserved byte -> PROTOCOL");
    hdr.reserved[71] = 0;
    AIRY_MEMCPY(buf, &hdr, sizeof(hdr));

    /* 严格长度：双向偏差均拒绝 */
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf) - 1, &view, &view_len, &trace,
                                          &src) == AIRY_EMSGSIZE,
                "one byte short -> EMSGSIZE");
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf) + 1, &view, &view_len, &trace,
                                          &src) == AIRY_EMSGSIZE,
                "one byte long -> EMSGSIZE");

    /* payload_len 超限 */
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    hdr.payload_len = DAEMON_L2_MAX_PAYLOAD + 1;
    AIRY_MEMCPY(buf, &hdr, sizeof(hdr));
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    AIRY_EMSGSIZE,
                "payload_len above cap -> EMSGSIZE");
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    hdr.payload_len = (__u32)payload_len;
    AIRY_MEMCPY(buf, &hdr, sizeof(hdr));

    /* payload 字节翻转：CRC 失配 */
    buf[AIRY_IPC_HDR_SIZE] ^= 0xFFu;
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    AIRY_ERR_CHECKSUM,
                "flipped payload byte -> CHECKSUM");
    buf[AIRY_IPC_HDR_SIZE] ^= 0xFFu;

    /* crc32 字段篡改 */
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));
    hdr.crc32 ^= 0xA5A5A5A5u;
    AIRY_MEMCPY(buf, &hdr, sizeof(hdr));
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), &view, &view_len, &trace, &src) ==
                    AIRY_ERR_CHECKSUM,
                "tampered crc32 field -> CHECKSUM");

    /* NULL 参数 */
    TEST_ASSERT(daemon_l2_envelope_decode(NULL, sizeof(buf), &view, &view_len, &trace,
                                          &src) == AIRY_EINVAL,
                "NULL buf rejected");
    TEST_ASSERT(daemon_l2_envelope_decode(buf, sizeof(buf), NULL, &view_len, &trace,
                                          &src) == AIRY_EINVAL,
                "NULL out_payload rejected");

    TEST_END();
}

/* ---- start 参数校验 ---- */

static int echo_dispatch(const char *req_json, size_t req_len, char **resp_json,
                         size_t *resp_len, void *userdata);

static void test_bridge_start_validation(void)
{
    TEST_BEGIN("test_bridge_start_validation");

    TEST_ASSERT(daemon_l2_bridge_start(NULL, echo_dispatch, NULL) == NULL,
                "NULL name rejected");
    TEST_ASSERT(daemon_l2_bridge_start("", echo_dispatch, NULL) == NULL,
                "empty name rejected");
    TEST_ASSERT(daemon_l2_bridge_start("l2t.valid", NULL, NULL) == NULL,
                "NULL dispatch rejected");

    daemon_l2_bridge_stop(NULL); /* NULL 安全：不得崩溃 */

    TEST_END();
}

/* ---- E2E：dispatch 回调与客户端 ---- */

struct echo_ctx {
    int hits;
    char last_req[64];
    size_t last_len;
};

static int echo_dispatch(const char *req_json, size_t req_len, char **resp_json,
                         size_t *resp_len, void *userdata)
{
    struct echo_ctx *ctx = (struct echo_ctx *)userdata;
    ctx->hits++;
    ctx->last_len = req_len < sizeof(ctx->last_req) ? req_len : sizeof(ctx->last_req) - 1;
    if (req_json) {
        AIRY_MEMCPY(ctx->last_req, req_json, ctx->last_len);
    }
    ctx->last_req[ctx->last_len] = '\0';

    /* 契约：resp_json 必须分配在 AIRY_MALLOC 域（bridge 以 AIRY_FREE 释放），
     * resp_len 不含 NUL。 */
    char *resp = (char *)AIRY_MALLOC(req_len + 1);
    if (!resp) {
        return -1;
    }
    for (size_t i = 0; i < req_len; i++) {
        char c = req_json[i];
        resp[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    resp[req_len] = '\0';
    *resp_json = resp;
    *resp_len = req_len;
    return 0;
}

static void test_e2e_echo_roundtrip(void)
{
    TEST_BEGIN("test_e2e_echo_roundtrip");

    struct echo_ctx ctx = {0};
    daemon_l2_bridge_t *bridge = daemon_l2_bridge_start("l2t.echo", echo_dispatch, &ctx);
    TEST_ASSERT(bridge != NULL, "bridge started");

    airy_ipc_channel_t *client = NULL;
    TEST_ASSERT(airy_ipc_connect("l2t.echo", &client) == AIRY_SUCCESS, "client connected");

    uint8_t req[AIRY_IPC_HDR_SIZE + 8];
    TEST_ASSERT(daemon_l2_envelope_encode(0xABCD1234ULL, l2t_getpid(), 999, "hello-l2", 8,
                                          req, sizeof(req)) == 0,
                "request envelope encoded");

    uint8_t resp[AIRY_IPC_HDR_SIZE + 64];
    size_t resp_size = sizeof(resp);
    airy_kernel_ipc_message_t msg = {.code = 1,
                                     .data = req,
                                     .size = sizeof(req),
                                     .fd = -1,
                                     .msg_id = 1};
    TEST_ASSERT(airy_ipc_call(client, &msg, resp, &resp_size, 2000) == AIRY_SUCCESS,
                "call succeeded");

    const void *resp_payload = NULL;
    size_t resp_payload_len = 0;
    uint64_t trace_id = 0;
    uint64_t src_task = 0;
    TEST_ASSERT(daemon_l2_envelope_decode(resp, resp_size, &resp_payload, &resp_payload_len,
                                          &trace_id, &src_task) == 0,
                "response envelope decoded");
    TEST_ASSERT(trace_id == 0xABCD1234ULL, "trace_id propagated end-to-end");
    TEST_ASSERT(src_task == l2t_getpid(), "src_task echoed back (same process)");
    TEST_ASSERT(resp_payload_len == 8, "response payload length");
    TEST_ASSERT(resp_payload && memcmp(resp_payload, "HELLO-L2", 8) == 0,
                "dispatch output carried in envelope payload");
    TEST_ASSERT(ctx.hits == 1, "dispatch invoked exactly once");
    TEST_ASSERT(ctx.last_len == 8 && memcmp(ctx.last_req, "hello-l2", 8) == 0,
                "request payload bytes reached dispatch");

    TEST_ASSERT(airy_ipc_close(client) == AIRY_SUCCESS, "client closed");
    daemon_l2_bridge_stop(bridge);
    airy_ipc_cleanup();

    TEST_END();
}

static void test_e2e_junk_envelope_dropped(void)
{
    TEST_BEGIN("test_e2e_junk_envelope_dropped");

    struct echo_ctx ctx = {0};
    daemon_l2_bridge_t *bridge = daemon_l2_bridge_start("l2t.junk", echo_dispatch, &ctx);
    TEST_ASSERT(bridge != NULL, "bridge started");

    airy_ipc_channel_t *client = NULL;
    TEST_ASSERT(airy_ipc_connect("l2t.junk", &client) == AIRY_SUCCESS, "client connected");

    static char junk[] = "totally-not-an-envelope";
    airy_kernel_ipc_message_t msg = {.code = 1,
                                     .data = junk,
                                     .size = sizeof(junk),
                                     .fd = -1,
                                     .msg_id = 1};
    uint8_t resp[256];
    size_t resp_size = sizeof(resp);
    TEST_ASSERT(airy_ipc_call(client, &msg, resp, &resp_size, 2000) == AIRY_ERR_CANCELED,
                "junk envelope dropped without reply (CANCELED)");
    TEST_ASSERT(ctx.hits == 0, "dispatch never invoked for junk");

    TEST_ASSERT(airy_ipc_close(client) == AIRY_SUCCESS, "client closed");
    daemon_l2_bridge_stop(bridge);
    airy_ipc_cleanup();

    TEST_END();
}

struct fail_ctx {
    int hits;
};

static int failing_dispatch(const char *req_json, size_t req_len, char **resp_json,
                            size_t *resp_len, void *userdata)
{
    struct fail_ctx *ctx = (struct fail_ctx *)userdata;
    ctx->hits++;
    (void)req_json;
    (void)req_len;
    (void)resp_json;
    (void)resp_len;
    return -1;
}

static void test_e2e_dispatch_failure(void)
{
    TEST_BEGIN("test_e2e_dispatch_failure");

    struct fail_ctx ctx = {0};
    daemon_l2_bridge_t *bridge = daemon_l2_bridge_start("l2t.fail", failing_dispatch, &ctx);
    TEST_ASSERT(bridge != NULL, "bridge started");

    airy_ipc_channel_t *client = NULL;
    TEST_ASSERT(airy_ipc_connect("l2t.fail", &client) == AIRY_SUCCESS, "client connected");

    uint8_t req[AIRY_IPC_HDR_SIZE + 4];
    TEST_ASSERT(daemon_l2_envelope_encode(7, 1, 2, "ping", 4, req, sizeof(req)) == 0,
                "request envelope encoded");

    uint8_t resp[AIRY_IPC_HDR_SIZE + 16];
    size_t resp_size = sizeof(resp);
    airy_kernel_ipc_message_t msg = {.code = 1,
                                     .data = req,
                                     .size = sizeof(req),
                                     .fd = -1,
                                     .msg_id = 1};
    TEST_ASSERT(airy_ipc_call(client, &msg, resp, &resp_size, 2000) == AIRY_ERR_CANCELED,
                "dispatch failure surfaced as CANCELED to the sender");
    TEST_ASSERT(ctx.hits == 1, "dispatch invoked once before failing");

    TEST_ASSERT(airy_ipc_close(client) == AIRY_SUCCESS, "client closed");
    daemon_l2_bridge_stop(bridge);
    airy_ipc_cleanup();

    TEST_END();
}

/* ---- 8.3.3：channel 派生 ---- */

static void test_channel_for_socket_derivation(void)
{
    TEST_BEGIN("test_channel_for_socket_derivation");

    char channel[64];

    /* 参数校验 */
    TEST_ASSERT(daemon_l2_channel_for_socket(NULL, channel, sizeof(channel)) == AIRY_EINVAL,
                "NULL socket_path rejected");
    TEST_ASSERT(daemon_l2_channel_for_socket("/run/airy/sched.sock", NULL, sizeof(channel)) ==
                    AIRY_EINVAL,
                "NULL channel rejected");
    TEST_ASSERT(daemon_l2_channel_for_socket("/run/airy/sched.sock", channel, 0) == AIRY_EINVAL,
                "zero size rejected");

    /* 非 ".sock" 命名：TCP endpoint、无后缀 basename、空 ns —— 一律留在
     * socket 路径（命名判别符语义，daemon_l2_bridge.c） */
    TEST_ASSERT(daemon_l2_channel_for_socket("127.0.0.1:8086", channel, sizeof(channel)) ==
                    AIRY_EINVAL,
                "TCP endpoint stays on the socket path");
    TEST_ASSERT(daemon_l2_channel_for_socket("/var/run/daemons", channel, sizeof(channel)) ==
                    AIRY_EINVAL,
                "basename without .sock rejected");
    TEST_ASSERT(daemon_l2_channel_for_socket("/run/airy/.sock", channel, sizeof(channel)) ==
                    AIRY_EINVAL,
                "empty namespace rejected");

    /* transport switch 关（默认 off）-> fail-closed：不发放无人挂载的 channel */
    unsetenv("AIRY_IPC_TRANSPORT");
    unsetenv("AIRY_SCHED_IPC_TRANSPORT");
    TEST_ASSERT(daemon_l2_channel_for_socket("/run/airy/sched.sock", channel,
                                             sizeof(channel)) == AIRY_ERR_NOT_FOUND,
                "switch off -> NOT_FOUND (grey coexistence norm)");

    /* scoped 开关放行 ns -> 派生 "<ns>.rpc" */
    setenv("AIRY_SCHED_IPC_TRANSPORT", "corekern", 1);
    TEST_ASSERT(daemon_l2_channel_for_socket("/run/airy/sched.sock", channel,
                                             sizeof(channel)) == 0,
                "scoped switch on -> channel derived");
    TEST_ASSERT(strcmp(channel, "sched.rpc") == 0, "channel is sched.rpc");

    /* 容量不足：守卫先于 transport 检查（"sched.rpc"+NUL 需 10 字节） */
    TEST_ASSERT(daemon_l2_channel_for_socket("/run/airy/sched.sock", channel, 5) == AIRY_EMSGSIZE,
                "undersized channel buffer -> EMSGSIZE");

    /* ns 过长：>= 64 字符触发 ns_upper 守卫 */
    char long_path[128];
    TEST_ASSERT(snprintf(long_path, sizeof(long_path), "/tmp/%064d.sock", 1) > 0,
                "over-long ns path built");
    TEST_ASSERT(daemon_l2_channel_for_socket(long_path, channel, sizeof(channel)) ==
                    AIRY_EMSGSIZE,
                "over-long namespace -> EMSGSIZE");

    unsetenv("AIRY_SCHED_IPC_TRANSPORT");

    TEST_END();
}

/* ---- 8.3.3：rpc_call / rpc_call_resp E2E ---- */

struct rpc_ctx {
    int hits;
    char last_method[32];
    char last_params[64];
    long last_id;
    int has_params;
};

/* JSON-RPC mock：解析请求（记录 method/id/params），按 method 应答。
 * 契约与 echo_dispatch 一致：resp_json 必须分配在 AIRY_MALLOC 域
 * （bridge 以 AIRY_FREE 释放），resp_len 不含 NUL。请求侧同契约：
 * 长度界定、无 NUL 保证，必须以长度感知 API 解析。 */
static int rpc_dispatch(const char *req_json, size_t req_len, char **resp_json,
                        size_t *resp_len, void *userdata)
{
    struct rpc_ctx *ctx = (struct rpc_ctx *)userdata;
    ctx->hits++;

    cJSON *req = cJSON_ParseWithLengthOpts(req_json, req_len, NULL, 0);
    if (!req) {
        return -1;
    }
    const cJSON *method = cJSON_GetObjectItem(req, "method");
    const cJSON *id = cJSON_GetObjectItem(req, "id");
    const cJSON *params = cJSON_GetObjectItem(req, "params");
    if (method && cJSON_IsString(method)) {
        snprintf(ctx->last_method, sizeof(ctx->last_method), "%s", method->valuestring);
    }
    if (id && cJSON_IsNumber(id)) {
        ctx->last_id = (long)id->valuedouble;
    }
    ctx->has_params = params != NULL;
    if (params) {
        char *ps = cJSON_PrintUnformatted((cJSON *)params);
        if (ps) {
            snprintf(ctx->last_params, sizeof(ctx->last_params), "%s", ps);
            AIRY_FREE(ps);
        }
    }

    const char *body = NULL;
    if (strcmp(ctx->last_method, "ok") == 0) {
        body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"pong\":true}}";
    } else if (strcmp(ctx->last_method, "bizerr") == 0) {
        body = "{\"jsonrpc\":\"2.0\",\"id\":1,"
               "\"error\":{\"code\":-32601,\"message\":\"no such method\"}}";
    } else if (strcmp(ctx->last_method, "nores") == 0) {
        body = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    } else if (strcmp(ctx->last_method, "garbage") == 0) {
        body = "not-json-at-all";
    } else if (strcmp(ctx->last_method, "empty") == 0) {
        /* 空响应：bridge 编码为空 payload envelope，发送方按折叠规则处理 */
        cJSON_Delete(req);
        *resp_json = NULL;
        *resp_len = 0;
        return 0;
    }
    cJSON_Delete(req);
    if (!body) {
        return -1;
    }

    size_t blen = strlen(body);
    char *resp = (char *)AIRY_MALLOC(blen + 1);
    if (!resp) {
        return -1;
    }
    AIRY_MEMCPY(resp, body, blen + 1);
    *resp_json = resp;
    *resp_len = blen;
    return 0;
}

static void test_rpc_call_e2e(void)
{
    TEST_BEGIN("test_rpc_call_e2e");

    /* 参数校验（不触达 transport；哨兵初值验证 out 被置 NULL） */
    char *out = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call(NULL, "m", NULL, &out, 1000) == AIRY_ERR_INVALID_PARAM &&
                    out == NULL,
                "NULL channel rejected");
    TEST_ASSERT(daemon_l2_rpc_call("l2t.rpcx", "m", NULL, NULL, 1000) == AIRY_ERR_INVALID_PARAM,
                "NULL out rejected");

    struct rpc_ctx ctx = {0};
    daemon_l2_bridge_t *bridge = daemon_l2_bridge_start("l2t.rpcx", rpc_dispatch, &ctx);
    TEST_ASSERT(bridge != NULL, "bridge started");

    /* result 解包 E2E */
    out = NULL;
    TEST_ASSERT(daemon_l2_rpc_call("l2t.rpcx", "ok", "{\"k\":1}", &out, 2000) == AIRY_SUCCESS,
                "ok method succeeded over L2");
    TEST_ASSERT(out && strcmp(out, "{\"pong\":true}") == 0, "result field extracted");
    AIRY_FREE(out);
    TEST_ASSERT(ctx.hits == 1, "dispatch invoked once");
    TEST_ASSERT(strcmp(ctx.last_method, "ok") == 0, "method reached dispatch");
    TEST_ASSERT(ctx.last_id == 1, "id fixed to 1 (rpc_connect_send parity)");
    TEST_ASSERT(ctx.has_params && strcmp(ctx.last_params, "{\"k\":1}") == 0,
                "valid JSON params embedded verbatim");

    /* params NULL -> {}（与 socket 路径请求序列化对齐） */
    out = NULL;
    TEST_ASSERT(daemon_l2_rpc_call("l2t.rpcx", "ok", NULL, &out, 2000) == AIRY_SUCCESS,
                "NULL params accepted");
    TEST_ASSERT(ctx.has_params && strcmp(ctx.last_params, "{}") == 0,
                "NULL params sent as {}");
    AIRY_FREE(out);

    /* daemon 业务错误 -> 折叠 GENERIC_FAIL（socket 路径同语义） */
    out = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call("l2t.rpcx", "bizerr", NULL, &out, 2000) ==
                        AIRY_ERR_GENERIC_FAIL &&
                    out == NULL,
                "daemon error reply folded to GENERIC_FAIL");

    /* 缺 result（也无 error）-> GENERIC_FAIL */
    out = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call("l2t.rpcx", "nores", NULL, &out, 2000) ==
                        AIRY_ERR_GENERIC_FAIL &&
                    out == NULL,
                "missing result folded to GENERIC_FAIL");

    /* 响应非 JSON -> GENERIC_FAIL */
    out = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call("l2t.rpcx", "garbage", NULL, &out, 2000) ==
                        AIRY_ERR_GENERIC_FAIL &&
                    out == NULL,
                "non-JSON reply folded to GENERIC_FAIL");

    /* 空 payload 响应 -> GENERIC_FAIL（空响应是畸形交换，非传输丢失） */
    out = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call("l2t.rpcx", "empty", NULL, &out, 2000) ==
                        AIRY_ERR_GENERIC_FAIL &&
                    out == NULL,
                "empty reply payload folded to GENERIC_FAIL");

    /* 未挂载 channel -> connect 失败码透传（corekern: ENOENT；8.3.3
     * 回退 socket 路径的依据，回退判定用 != SUCCESS 不依赖具体码） */
    out = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call("l2t.nomount", "ok", NULL, &out, 2000) == AIRY_ENOENT &&
                    out == NULL,
                "unmounted channel propagates connect error (ENOENT)");

    daemon_l2_bridge_stop(bridge);
    airy_ipc_cleanup();

    TEST_END();
}

static void test_rpc_call_resp_e2e(void)
{
    TEST_BEGIN("test_rpc_call_resp_e2e");

    /* NULL out 校验 */
    TEST_ASSERT(daemon_l2_rpc_call_resp("l2t.respx", "ok", NULL, 1000, NULL) ==
                    AIRY_ERR_INVALID_PARAM,
                "NULL out rejected");

    struct rpc_ctx ctx = {0};
    daemon_l2_bridge_t *bridge = daemon_l2_bridge_start("l2t.respx", rpc_dispatch, &ctx);
    TEST_ASSERT(bridge != NULL, "bridge started");

    /* 业务错误原样透传：SUCCESS + 完整 JSON-RPC 响应 —— gateway 双路
     * 分发（gw_svc_call L2 先行路）与 socket 路径位对位的核心契约 */
    char *resp = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call_resp("l2t.respx", "bizerr", NULL, 2000, &resp) == AIRY_SUCCESS,
                "daemon error reply is transport SUCCESS");
    TEST_ASSERT(resp && strstr(resp, "\"error\"") && strstr(resp, "-32601") &&
                    strstr(resp, "no such method"),
                "error object carried verbatim");
    AIRY_FREE(resp);

    /* 成功响应完整透传；timeout 0 -> 默认 30s 归一路径 */
    resp = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call_resp("l2t.respx", "ok", NULL, 0, &resp) == AIRY_SUCCESS,
                "timeout 0 -> default 30s path works");
    TEST_ASSERT(resp && strstr(resp, "\"result\"") && strstr(resp, "\"jsonrpc\""),
                "complete response carried verbatim");
    AIRY_FREE(resp);

    /* 未挂载 channel -> connect 失败码透传 + out NULL */
    resp = (char *)0x1;
    TEST_ASSERT(daemon_l2_rpc_call_resp("l2t.nomount", "ok", NULL, 2000, &resp) == AIRY_ENOENT &&
                    resp == NULL,
                "unmounted channel propagates connect error (ENOENT)");

    daemon_l2_bridge_stop(bridge);
    airy_ipc_cleanup();

    TEST_END();
}

int main(void)
{
    printf("========================================\n");
    printf("  daemon_l2_bridge unit tests (8.3.2)\n");
    printf("========================================\n");

    test_encode_param_validation();
    test_envelope_roundtrip_fields();
    test_envelope_empty_payload();
    test_decode_corruption();
    test_bridge_start_validation();
    test_e2e_echo_roundtrip();
    test_e2e_junk_envelope_dropped();
    test_e2e_dispatch_failure();
    test_channel_for_socket_derivation();
    test_rpc_call_e2e();
    test_rpc_call_resp_e2e();

    printf("\n========================================\n");
    printf("  daemon_l2_bridge 测试结果汇总\n");
    printf("========================================\n");
    printf("  通过:   %d\n", g_tests_passed);
    printf("  失败:   %d\n", g_tests_failed);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
