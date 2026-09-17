// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_provider_http_dispatch.c
 * @brief 运行级回归：provider HTTP 传输层分诊与出网策略（端到端）。
 *
 * 单元测试此前只覆盖请求构造/响应解析（test_provider_reasoning.c），没有
 * 任何用例真正把 provider 跑到 HTTP 传输层。于是 "400 请求体被拒" 与
 * "网络不可达" 是否被混为一谈、流式路径是否也走了专用码、出网代理是否
 * 真被注入句柄，均无回归保护。
 *
 * 本测试在进程内起一个最小 HTTP 桩（POSIX 线程 + AF_INET 回环临时端口，
 * 零外部依赖），驱动 openai_ops.complete / complete_stream 走完整链路：
 *
 *   1. 400/422 → AIRY_ERR_LLM_BAD_REQUEST（确定性失败，见 openai_rate_limit.c
 *      与 provider_stream.c 的 N-3 映射）
 *   2. 401     → AIRY_ERR_LLM_AUTH_FAIL（反向对照：证明分诊不是"失败即 400"）
 *   3. 连接被拒（http_code==0）→ AIRY_ERR_IO，与 400 明确区分
 *   4. 200 正向对照（非流式 + 流式 SSE），证明桩确实能让成功路径通过，
 *      排除"桩恒返回失败导致断言伪绿"
 *   5. 出网策略：失败分诊映射表；以及死代理下的失败／清除代理后的成功
 *      对照，证明 AIRY_HTTP_PROXY 确被注入句柄（N-1/N-2）
 *   6. 出网重试：瞬时故障（对端接受后立即关闭）必须真的重发，尝试次数严格
 *      等于 max_retries；流式在首包下发前同样重发且不得交付半截输出
 *      （N-3/N-4）
 *
 * 平台：daemon 测试在 Windows 整体关闭（daemons/CMakeLists.txt 的 WIN32 门），
 * 故本文件直接使用 POSIX socket/pthread，无需跨平台分支。
 */

#include "daemon_platform_ext.h"
#include "error.h"
#include "provider.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* openai_ops 定义于 openai.c:182，未在头文件声明（内部符号），此处 extern。 */
extern const provider_ops_t openai_ops;

/* ── 极简 HTTP 桩 ─────────────────────────────────────────────────────── */

typedef struct {
    int listen_fd;
    int port;
    pthread_t tid;
    pthread_mutex_t lock;
    _Atomic int stop;
    _Atomic int drop;        /* 1 = 接受后立即关闭连接（模拟瞬时对端故障） */
    _Atomic int conn_count;  /* 已接受的连接数：用于证明重试真的发生了 */
    char status_line[64];
    char content_type[64];
    char *body;
} mock_server_t;

static mock_server_t g_srv;

static void mock_set_drop(int on)
{
    g_srv.drop = on;
}

static void mock_set_response(int code, const char *reason, const char *content_type,
                              const char *body)
{
    pthread_mutex_lock(&g_srv.lock);
    snprintf(g_srv.status_line, sizeof(g_srv.status_line), "HTTP/1.1 %d %s", code, reason);
    snprintf(g_srv.content_type, sizeof(g_srv.content_type), "%s",
             content_type ? content_type : "application/json");
    free(g_srv.body);
    g_srv.body = strdup(body ? body : "");
    pthread_mutex_unlock(&g_srv.lock);
}

/* Case-insensitive substring search (request headers are ASCII). */
static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               (p[i] == needle[i] || (p[i] | 0x20) == (needle[i] | 0x20)))
            i++;
        if (i == nlen)
            return p;
    }
    return NULL;
}

static void mock_serve_one(int fd)
{
    char req[8192];
    size_t used = 0;
    const char *hdr_end = NULL;

    /* Read request headers up to CRLFCRLF, then drain the declared body so the
     * client never blocks on a full receive buffer. */
    while (used + 1 < sizeof(req)) {
        ssize_t n = recv(fd, req + used, sizeof(req) - 1 - used, 0);
        if (n <= 0)
            break;
        used += (size_t)n;
        req[used] = '\0';
        hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end)
            break;
    }

    if (hdr_end) {
        size_t header_len = (size_t)(hdr_end - req) + 4;
        const char *cl = ci_strstr(req, "Content-Length:");
        long clen = cl ? strtol(cl + strlen("Content-Length:"), NULL, 10) : 0;
        long have = (long)(used > header_len ? used - header_len : 0);
        while (have < clen) {
            ssize_t m = recv(fd, req, sizeof(req), 0);
            if (m <= 0)
                break;
            have += (long)m;
        }
    }

    char *resp = NULL;
    size_t resp_len = 0;
    pthread_mutex_lock(&g_srv.lock);
    {
        const char *b = g_srv.body ? g_srv.body : "";
        size_t blen = strlen(b);
        size_t cap = strlen(g_srv.status_line) + strlen(g_srv.content_type) + blen + 256;
        resp = (char *)malloc(cap);
        if (resp) {
            resp_len = (size_t)snprintf(resp, cap,
                                        "%s\r\n"
                                        "Content-Type: %s\r\n"
                                        "Content-Length: %zu\r\n"
                                        "Connection: close\r\n"
                                        "\r\n"
                                        "%s",
                                        g_srv.status_line, g_srv.content_type, blen, b);
        }
    }
    pthread_mutex_unlock(&g_srv.lock);

    if (resp) {
        size_t off = 0;
        while (off < resp_len) {
            ssize_t w = send(fd, resp + off, resp_len - off, 0);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        free(resp);
    }
}

static void *mock_server_thread(void *arg)
{
    (void)arg;
    while (!g_srv.stop) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int fd = accept(g_srv.listen_fd, (struct sockaddr *)&cli, &clen);
        if (fd < 0) {
            if (g_srv.stop)
                break;
            continue; /* EINTR / transient accept error */
        }
        atomic_fetch_add(&g_srv.conn_count, 1);
        if (g_srv.drop) {
            /* 连接已建立但一个字节都不回：curl 报 GOT_NOTHING/RECV_ERROR，
             * 属可重试的瞬时故障（用于 N-3/N-4 重试回归）。 */
            close(fd);
            continue;
        }
        mock_serve_one(fd);
        close(fd);
    }
    return NULL;
}

static int mock_server_start(void)
{
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.listen_fd = -1;
    pthread_mutex_init(&g_srv.lock, NULL);
    g_srv.body = strdup("");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }

    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(fd);
        return -1;
    }

    g_srv.listen_fd = fd;
    g_srv.port = ntohs(addr.sin_port);

    if (pthread_create(&g_srv.tid, NULL, mock_server_thread, NULL) != 0) {
        close(fd);
        g_srv.listen_fd = -1;
        return -1;
    }
    return 0;
}

static void mock_server_stop(void)
{
    g_srv.stop = 1;
    if (g_srv.listen_fd >= 0) {
        shutdown(g_srv.listen_fd, SHUT_RDWR);
        close(g_srv.listen_fd);
    }
    /* join 建立同步边沿后再置 -1，避免与 accept(g_srv.listen_fd) 竞态 */
    pthread_join(g_srv.tid, NULL);
    g_srv.listen_fd = -1;
    pthread_mutex_destroy(&g_srv.lock);
    free(g_srv.body);
    g_srv.body = NULL;
}

/* Pick a loopback port that is currently unbound (for the unreachable case). */
static int find_unused_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(fd);
        return -1;
    }
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

/* ── 测试用例 ─────────────────────────────────────────────────────────── */

/* Provider 400 错误体（OpenAI 兼容形状；不用于解析，仅用于日志/透传） */
static const char *const ERR_REJECT_BODY =
    "{\"error\":{\"message\":\"Invalid request body: invalid unicode code point\","
    "\"type\":\"invalid_request_error\",\"code\":\"invalid_body\"}}";

static const char *const ERR_AUTH_BODY =
    "{\"error\":{\"message\":\"Incorrect API key provided\","
    "\"type\":\"invalid_request_error\",\"code\":\"invalid_api_key\"}}";

static const char *const OK_BODY =
    "{\"id\":\"chatcmpl-dispatch\",\"object\":\"chat.completion\",\"model\":\"gpt-3.5-turbo\","
    "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"pong\"},"
    "\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1,\"total_tokens\":4}}";

static const char *const SSE_OK_BODY =
    "data: {\"id\":\"chatcmpl-s\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-3.5-turbo\","
    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"pong\"},\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"chatcmpl-s\",\"object\":\"chat.completion.chunk\",\"model\":\"gpt-3.5-turbo\","
    "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

static int g_stream_chunk_count;

static void on_stream_chunk(const char *chunk, void *user_data)
{
    (void)user_data;
    if (chunk && chunk[0] != '\0')
        g_stream_chunk_count++;
}

static void cfg_init(llm_request_config_t *cfg, llm_message_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->role = "user";
    msg->content = "hello";

    memset(cfg, 0, sizeof(*cfg));
    cfg->model = "gpt-3.5-turbo";
    cfg->messages = msg;
    cfg->message_count = 1;
    cfg->max_tokens = 16;
    cfg->temperature = 0.0f;
}

/* max_retries=1 → openai_http_request_with_retry 只发一次请求，测试确定。 */
static provider_ctx_t *make_ctx(const char *base_url)
{
    provider_ctx_t *ctx = openai_ops.init("openai", "", base_url, NULL, 5.0, 1);
    assert(ctx != NULL);
    return ctx;
}

static void test_nonstream_dispatch(int code, const char *reason, const char *body, int expect,
                                    const char *desc)
{
    printf("  %s...\n", desc);
    mock_set_response(code, reason, "application/json", body);

    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", g_srv.port);

    provider_ctx_t *ctx = make_ctx(base);
    llm_message_t msg;
    llm_request_config_t cfg;
    cfg_init(&cfg, &msg);

    llm_response_t *resp = NULL;
    int ret = openai_ops.complete(ctx, &cfg, &resp);

    printf("    http=%d ret=%d expect=%d\n", code, ret, expect);
    assert(ret == expect);

    if (expect == AIRY_OK) {
        assert(resp != NULL);
        assert(resp->choice_count == 1);
        assert(resp->choices[0].content != NULL);
        assert(strcmp(resp->choices[0].content, "pong") == 0);
    }
    if (resp)
        llm_response_free(resp);

    openai_ops.destroy(ctx);
}

static void test_stream_dispatch(int code, const char *reason, const char *content_type,
                                 const char *body, int expect, const char *desc)
{
    printf("  %s...\n", desc);
    mock_set_response(code, reason, content_type, body);

    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", g_srv.port);

    provider_ctx_t *ctx = make_ctx(base);
    llm_message_t msg;
    llm_request_config_t cfg;
    cfg_init(&cfg, &msg);

    g_stream_chunk_count = 0;
    llm_response_t *resp = NULL;
    int ret = openai_ops.complete_stream(ctx, &cfg, on_stream_chunk, NULL, &resp);

    printf("    http=%d ret=%d expect=%d chunks=%d\n", code, ret, expect, g_stream_chunk_count);
    assert(ret == expect);

    if (expect == AIRY_OK) {
        assert(resp != NULL);
        assert(resp->choice_count == 1);
        assert(resp->choices[0].content != NULL);
        assert(strcmp(resp->choices[0].content, "pong") == 0);
        assert(g_stream_chunk_count >= 1); /* 增量确实透出给了流回调 */
    }
    if (resp)
        llm_response_free(resp);

    openai_ops.destroy(ctx);
}

/* ── N-1/N-2：出网传输策略 SSoT（provider_http_setup / provider_http_diag） ── */

static const char *const PROXY_ENV_NAMES[] = {
    "AIRY_HTTP_PROXY", "HTTP_PROXY",  "http_proxy",
    "HTTPS_PROXY",     "https_proxy", "ALL_PROXY",
    "all_proxy",
};

static void test_http_diag_map(void)
{
    printf("  outbound failure classification...\n");

    assert(strcmp(provider_http_diag(CURLE_COULDNT_RESOLVE_HOST), "DNS_FAIL") == 0);
    assert(strcmp(provider_http_diag(CURLE_COULDNT_RESOLVE_PROXY), "DNS_FAIL") == 0);
    assert(strcmp(provider_http_diag(CURLE_COULDNT_CONNECT), "CONNECT_FAIL") == 0);
    assert(strcmp(provider_http_diag(CURLE_OPERATION_TIMEDOUT), "TIMEOUT") == 0);
    assert(strcmp(provider_http_diag(CURLE_PEER_FAILED_VERIFICATION), "TLS_FAIL") == 0);
    assert(strcmp(provider_http_diag(CURLE_SSL_CONNECT_ERROR), "TLS_FAIL") == 0);
    assert(strcmp(provider_http_diag(CURLE_PROXY), "PROXY_FAIL") == 0);
    assert(strcmp(provider_http_diag(CURLE_RECV_ERROR), "NET_IO_FAIL") == 0);
    assert(strcmp(provider_http_diag(CURLE_GOT_NOTHING), "OTHER") == 0);
    assert(provider_http_diag((CURLcode)0x7fffffff) != NULL);
}

/* N-2：AIRY_HTTP_PROXY 是 agentrt 自有变量，libcurl 不会自行读取。因此
 * "设一个死代理 → 本可成功的请求必须失败；清除后必须恢复成功" 只有在
 * provider_http_setup() 确实注入了 CURLOPT_PROXY 时才成立。宿主环境中的
 * 标准代理变量先被隔离，确保观察到的行为只来自 AIRY_HTTP_PROXY。 */
static void test_proxy_env_injected(void)
{
    printf("  outbound proxy injected (AIRY_HTTP_PROXY)...\n");

    char saved[7][512];
    int saved_set[7];
    const size_t nenv = sizeof(PROXY_ENV_NAMES) / sizeof(PROXY_ENV_NAMES[0]);
    for (size_t i = 0; i < nenv; i++) {
        const char *v = getenv(PROXY_ENV_NAMES[i]);
        saved_set[i] = (v && v[0]) ? 1 : 0;
        saved[i][0] = '\0';
        if (saved_set[i]) {
            strncpy(saved[i], v, sizeof(saved[i]) - 1);
            saved[i][sizeof(saved[i]) - 1] = '\0';
        }
        unsetenv(PROXY_ENV_NAMES[i]);
    }

    int dead_port = find_unused_port();
    assert(dead_port > 0);

    char proxy[64];
    snprintf(proxy, sizeof(proxy), "http://127.0.0.1:%d", dead_port);

    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", g_srv.port);
    mock_set_response(200, "OK", "application/json", OK_BODY);

    llm_message_t msg;
    llm_request_config_t cfg;
    llm_response_t *resp = NULL;

    assert(setenv("AIRY_HTTP_PROXY", proxy, 1) == 0);
    provider_ctx_t *ctx = make_ctx(base);
    cfg_init(&cfg, &msg);
    int ret = openai_ops.complete(ctx, &cfg, &resp);
    printf("    dead proxy  ret=%d expect=%d\n", ret, AIRY_ERR_IO);
    assert(ret == AIRY_ERR_IO);
    if (resp)
        llm_response_free(resp);
    openai_ops.destroy(ctx);

    unsetenv("AIRY_HTTP_PROXY");

    ctx = make_ctx(base);
    cfg_init(&cfg, &msg);
    resp = NULL;
    ret = openai_ops.complete(ctx, &cfg, &resp);
    printf("    no proxy    ret=%d expect=%d\n", ret, AIRY_OK);
    assert(ret == AIRY_OK);
    if (resp)
        llm_response_free(resp);
    openai_ops.destroy(ctx);

    for (size_t i = 0; i < nenv; i++)
        if (saved_set[i])
            setenv(PROXY_ENV_NAMES[i], saved[i], 1);
}

/* ── N-3/N-4：出网重试策略 SSoT（provider_retryable / backoff / budget） ── */

static uint32_t ideal_backoff(int attempt)
{
    uint32_t b = PROVIDER_RETRY_BASE_MS;
    for (int i = 0; i < attempt && b < PROVIDER_RETRY_MAX_MS; i++)
        b <<= 1;
    return b > PROVIDER_RETRY_MAX_MS ? PROVIDER_RETRY_MAX_MS : b;
}

static void test_retry_policy(void)
{
    printf("  retry policy: classification + backoff bounds...\n");

    /* 瞬时传输故障必须重试 */
    assert(provider_retryable(CURLE_COULDNT_RESOLVE_PROXY) == 1);
    assert(provider_retryable(CURLE_COULDNT_RESOLVE_HOST) == 1);
    assert(provider_retryable(CURLE_COULDNT_CONNECT) == 1);
    assert(provider_retryable(CURLE_OPERATION_TIMEDOUT) == 1);
    assert(provider_retryable(CURLE_SSL_CONNECT_ERROR) == 1);
    assert(provider_retryable(CURLE_GOT_NOTHING) == 1);
    assert(provider_retryable(CURLE_SEND_ERROR) == 1);
    assert(provider_retryable(CURLE_PARTIAL_FILE) == 1);
    assert(provider_retryable(CURLE_RECV_ERROR) == 1);

    /* 确定性失败与调用方主动取消不得消耗预算 */
    assert(provider_retryable(CURLE_URL_MALFORMAT) == 0);
    assert(provider_retryable(CURLE_UNSUPPORTED_PROTOCOL) == 0);
    assert(provider_retryable(CURLE_WRITE_ERROR) == 0);
    assert(provider_retryable(CURLE_PEER_FAILED_VERIFICATION) == 0);

    /* 退避：随机抖动必须落在 [base-jitter, base+jitter]，且整体受上限约束 */
    for (int a = 0; a < 8; a++) {
        uint32_t ideal = ideal_backoff(a);
        uint32_t jit = ideal * PROVIDER_RETRY_JITTER_PCT / 100U;
        for (int s = 0; s < 64; s++) {
            uint32_t v = provider_backoff_ms(a);
            assert(v >= ideal - jit);
            assert(v <= ideal + jit);
            assert(v <= PROVIDER_RETRY_MAX_MS +
                            PROVIDER_RETRY_MAX_MS * PROVIDER_RETRY_JITTER_PCT / 100U);
        }
    }

    /* 抖动必须真的随时间变化：否则所有客户端同相位重试，"退避"退化为重试风暴 */
    {
        uint32_t first = provider_backoff_ms(2);
        int varied = 0;
        for (int s = 0; s < 64 && !varied; s++)
            if (provider_backoff_ms(2) != first)
                varied = 1;
        assert(varied);
    }

    /* 首次尝试的退避不得为 0：否则重试与"立即重打"无差别 */
    assert(provider_backoff_ms(0) > 0);
    assert(ideal_backoff(3) > ideal_backoff(0));

    /* 预算：耗尽即为 0（不得为负，否则外层会用负超时继续尝试）。
     * 断言一律以实测起点取差值，不依赖 airy_time_ms 的零点量级。 */
    uint64_t t0 = airy_time_ms();
    assert(provider_left_sec(0.0, t0) == 0.0);
    assert(provider_left_sec(1.0, t0) > 0.0);
    assert(provider_left_sec(1.0, t0) <= 1.0);
    assert(provider_left_sec(60.0, t0) > 59.0);
    assert(provider_left_sec(1.0, t0 - 2000U) == 0.0);

    /* 重试门槛：退避等待之后必须还放得下一次最小尝试窗口，否则不得重试。
     * 这条判据此前用"剩余 >= 5s"的绝对下限，预算 <= 5s 时任何重试都被否决
     * （重试策略形同虚设），故改为"退避 + 最小窗口"的相对判据。 */
    assert(provider_retry_budget_ok(60.0, t0, 200U) == 1);
    assert(provider_retry_budget_ok(5.0, t0, 2000U) == 1);
    assert(provider_retry_budget_ok(2.0, t0, 2000U) == 0);
    assert(provider_retry_budget_ok(1.0, t0, 200U) == 0);
    assert(provider_retry_budget_ok(1.0, t0 - 500U, 800U) == 0);
    assert(provider_retry_budget_ok(1.0, t0 - 2000U, 200U) == 0);
}

/* N-3 端到端：对端"接受后立即关闭"是典型瞬时故障；非流式路径必须真的重发，
 * 且总尝试次数严格等于 max_retries（不得退化为无限重试或零重试）。 */
static void test_nonstream_transient_retried(void)
{
    printf("  nonstream transient failure is retried...\n");

    mock_set_drop(1);
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", g_srv.port);

    provider_ctx_t *ctx = openai_ops.init("openai", "", base, NULL, 5.0, 2);
    assert(ctx != NULL);

    llm_message_t msg;
    llm_request_config_t cfg;
    cfg_init(&cfg, &msg);

    int before = atomic_load(&g_srv.conn_count);
    llm_response_t *resp = NULL;
    int ret = openai_ops.complete(ctx, &cfg, &resp);
    int attempts = atomic_load(&g_srv.conn_count) - before;
    if (resp)
        llm_response_free(resp);

    printf("    ret=%d attempts=%d expect=%d\n", ret, attempts, 3);
    assert(ret == AIRY_ERR_IO);
    /* max_retries 是"额外重试次数"：初次 + 2 次退避重试，既不得退化为
     * 无限重试，也不得退化为零重试。 */
    assert(attempts == 3);

    openai_ops.destroy(ctx);
    mock_set_drop(0);
}

/* N-4 端到端：流式路径在"首包未下发"时必须同样重发（此前完全无重试）。 */
static void test_stream_transient_retried(void)
{
    printf("  stream transient failure before first byte is retried...\n");

    mock_set_drop(1);
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", g_srv.port);

    provider_ctx_t *ctx = openai_ops.init("openai", "", base, NULL, 5.0, 2);
    assert(ctx != NULL);

    llm_message_t msg;
    llm_request_config_t cfg;
    cfg_init(&cfg, &msg);

    int before = atomic_load(&g_srv.conn_count);
    g_stream_chunk_count = 0;
    llm_response_t *resp = NULL;
    int ret = openai_ops.complete_stream(ctx, &cfg, on_stream_chunk, NULL, &resp);
    int attempts = atomic_load(&g_srv.conn_count) - before;
    if (resp)
        llm_response_free(resp);

    printf("    ret=%d attempts=%d expect=%d\n", ret, attempts, 3);
    assert(ret == AIRY_ERR_IO);
    assert(attempts == 3);
    assert(g_stream_chunk_count == 0); /* 首包未下发，用户不得看到任何半截输出 */

    openai_ops.destroy(ctx);
    mock_set_drop(0);
}

static void test_unreachable_is_io(void)
{
    printf("  nonstream connection-refused -> IO...\n");

    int port = find_unused_port();
    assert(port > 0);

    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);

    provider_ctx_t *ctx = make_ctx(base);
    llm_message_t msg;
    llm_request_config_t cfg;
    cfg_init(&cfg, &msg);

    llm_response_t *resp = NULL;
    int ret = openai_ops.complete(ctx, &cfg, &resp);
    if (resp)
        llm_response_free(resp);

    printf("    ret=%d expect=%d\n", ret, AIRY_ERR_IO);
    /* 核心：不可达必须与"请求体被拒"区分，不得退化为 BAD_REQUEST。 */
    assert(ret == AIRY_ERR_IO);
    assert(ret != AIRY_ERR_LLM_BAD_REQUEST);

    openai_ops.destroy(ctx);
}

int main(void)
{
    printf("=========================================\n");
    printf("  Provider HTTP status -> error dispatch\n");
    printf("=========================================\n");

    /* 桩在客户端提前关闭连接时会写失败；忽略 SIGPIPE 以免测试进程被杀。 */
    signal(SIGPIPE, SIG_IGN);

    if (mock_server_start() != 0) {
        printf("FATAL: cannot start mock HTTP server\n");
        return 1;
    }
    printf("  mock server listening on 127.0.0.1:%d\n", g_srv.port);

    /* N-3 主断言：400/422 归入专用码（非流式全链路）。 */
    test_nonstream_dispatch(400, "Bad Request", ERR_REJECT_BODY, AIRY_ERR_LLM_BAD_REQUEST,
                            "nonstream 400 -> BAD_REQUEST");
    test_nonstream_dispatch(422, "Unprocessable Entity", ERR_REJECT_BODY,
                            AIRY_ERR_LLM_BAD_REQUEST, "nonstream 422 -> BAD_REQUEST");

    /* 反向对照：401 仍走鉴权码，证明映射是按状态码分诊而非一律 400。 */
    test_nonstream_dispatch(401, "Unauthorized", ERR_AUTH_BODY, AIRY_ERR_LLM_AUTH_FAIL,
                            "nonstream 401 -> AUTH_FAIL");

    /* 正向对照：200 必须成功，排除桩恒失败造成的伪绿。 */
    test_nonstream_dispatch(200, "OK", OK_BODY, AIRY_OK, "nonstream 200 -> OK");

    /* N-3 流式：400 同样归入专用码（SSE 传输层 + openai_stream 双重映射）。 */
    test_stream_dispatch(400, "Bad Request", "application/json", ERR_REJECT_BODY,
                         AIRY_ERR_LLM_BAD_REQUEST, "stream 400 -> BAD_REQUEST");

    /* 流式正向对照：SSE 增量解析 + [DONE] 正常收尾。 */
    test_stream_dispatch(200, "OK", "text/event-stream", SSE_OK_BODY, AIRY_OK,
                         "stream 200 -> OK");

    /* 网络不可达与请求体被拒必须可区分。 */
    test_unreachable_is_io();

    /* N-1/N-2：出网传输策略 SSoT 的分诊与代理注入。 */
    test_http_diag_map();
    test_proxy_env_injected();

    /* N-3/N-4：重试策略 SSoT，以及瞬时故障下"重试真的发生"。 */
    test_retry_policy();
    test_nonstream_transient_retried();
    test_stream_transient_retried();

    mock_server_stop();

    printf("\nAll provider HTTP dispatch tests PASSED\n");
    return 0;
}
