// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_agent_review.c
 * @brief R16 语义复核挂载点回归：真实 AF_UNIX 假 think_d 往返，非桩。
 *
 * 各断言守护：
 *   T1 挂载点真的走 pipeline 档——请求体必须携带 mode=pipeline，否则
 *      think_d 侧整链复核不会被驱动；
 *   T2 判定搬运——DRIFT 与 REJECT 原样回到调用方；
 *   T3 不可达降级——无 think_d 时返回 SKIP 且不阻塞调用方；
 *   T4 空正文——门禁前置拒绝，返回 -1 且不发请求。
 */

#include "agent_review.h"

#include "platform_paths.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    int fd;
    const char *reply;
    char req[4096];
    int got;
} srv_t;

static void *srv_run(void *arg)
{
    srv_t *s = (srv_t *)arg;
    int c = accept(s->fd, NULL, NULL);
    if (c >= 0) {
        ssize_t n = recv(c, s->req, sizeof(s->req) - 1, 0);
        if (n > 0)
            s->req[n] = '\0';
        (void)send(c, s->reply, strlen(s->reply), 0);
        close(c);
        s->got = 1;
    }
    return NULL;
}

static int srv_open(srv_t *s, const char *path, const char *reply)
{
    struct sockaddr_un a;
    memset(s, 0, sizeof(*s));
    s->reply = reply;
    s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    unlink(path);
    if (bind(s->fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s->fd, 2) != 0) {
        close(s->fd);
        return -1;
    }
    return 0;
}

static void srv_close(srv_t *s, const char *path)
{
    close(s->fd);
    unlink(path);
}

static void test_pipeline(const char *path, const char *reply, int want)
{
    srv_t s;
    pthread_t th;
    if (srv_open(&s, path, reply) != 0) {
        fprintf(stderr, "FAIL: mock think_d bind %s\n", path);
        exit(1);
    }
    pthread_create(&th, NULL, srv_run, &s);

    agent_review_result_t rv;
    int rc = agent_rev_apply("goal-x", "body-y", "{\"text\":\"body-y\"}", "{\"ok\":true}", &rv);

    pthread_join(th, NULL);
    srv_close(&s, path);
    if (!s.got || !strstr(s.req, "\"mode\":\"pipeline\"")) {
        fprintf(stderr, "FAIL: request did not ask for the pipeline: %s\n", s.req);
        exit(1);
    }
    if (rc != 0 || rv.verdict != want) {
        fprintf(stderr, "FAIL: rc=%d verdict=%d want=%d reason=%s\n", rc, rv.verdict, want,
                rv.reason);
        exit(1);
    }
}

int main(void)
{
    char dir[] = "/tmp/airy-rev-XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }
    setenv("AIRY_RUNTIME_DIR", dir, 1);

    char sock[256];
    snprintf(sock, sizeof(sock), "%s/think.sock", dir);
    printf("  socket: %s\n", sock);

    printf("  test_pipeline_drift...\n");
    test_pipeline(sock, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"verdict\":1,\"reason\":\"d1\"}}",
                  AIRY_REVIEW_DRIFT);

    printf("  test_pipeline_reject...\n");
    test_pipeline(sock, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"verdict\":2,\"reason\":\"bad\"}}",
                  AIRY_REVIEW_REJECT);

    printf("  test_degrade_unreachable...\n");
    agent_review_result_t rv;
    int rc = agent_rev_apply("goal", "body", NULL, NULL, &rv);
    assert(rc == 0 && rv.verdict == AIRY_REVIEW_SKIP);

    printf("  test_empty_body...\n");
    assert(agent_rev_apply("goal", "", NULL, NULL, &rv) == -1);
    assert(agent_rev_apply("goal", NULL, NULL, NULL, &rv) == -1);

    rmdir(dir);
    printf("ALL PASSED\n");
    return 0;
}
