// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service.c
 * @brief Agent 服务单元测试
 */

#include "agent_service.h"
#include "service.h"

#include "airy_memory.h"

#include <assert.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* fork/pipe/dup2 */

static void test_create_destroy(void)
{
    printf("  test_create_destroy...\n");

    agent_service_t *svc = agent_service_create(0);
    assert(svc != NULL);
    assert(agent_service_count(svc) == 0);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_spawn_and_list(void)
{
    printf("  test_spawn_and_list...\n");

    agent_service_t *svc = agent_service_create(16);
    assert(svc != NULL);

    const char *spec = "{\"type\":\"echo\",\"model\":\"gpt-4\"}";
    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, spec, &agent_id);
    assert(ret == AIRY_SUCCESS);
    assert(agent_id != NULL);
    assert(strlen(agent_id) == 32);

    assert(agent_service_count(svc) == 1);

    char **ids = NULL;
    size_t count = 0;
    ret = agent_service_list(svc, &ids, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 1);
    assert(ids != NULL);
    assert(ids[0] != NULL);
    assert(strcmp(ids[0], agent_id) == 0);

    agent_service_list_free(ids, count);
    AIRY_FREE(agent_id);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_terminate(void)
{
    printf("  test_terminate...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, "{\"type\":\"worker\"}", &agent_id);
    assert(ret == AIRY_SUCCESS);
    assert(agent_id != NULL);

    ret = agent_service_terminate(svc, agent_id);
    assert(ret == AIRY_SUCCESS);

    assert(agent_service_count(svc) == 1);

    char *out_output = NULL;
    ret = agent_service_invoke(svc, agent_id, "ping", 4, NULL, &out_output);
    assert(ret != AIRY_SUCCESS);
    assert(out_output != NULL);
    assert(strstr(out_output, "error") != NULL);
    AIRY_FREE(out_output);

    AIRY_FREE(agent_id);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_invoke(void)
{
    printf("  test_invoke...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, "{\"type\":\"echo\"}", &agent_id);
    assert(ret == AIRY_SUCCESS);
    assert(agent_id != NULL);

    char *out_output = NULL;
    /* P0-2: in AIRY_AGENT_NO_SPAWN mode there is no child process; invoke
     * must return a clear error rather than an "invocation processed" fake
     * success */
    ret = agent_service_invoke(svc, agent_id, "hello", 5, NULL, &out_output);
    assert(ret != AIRY_SUCCESS);
    assert(out_output != NULL);
    assert(strstr(out_output, "error") != NULL);

    AIRY_FREE(out_output);
    AIRY_FREE(agent_id);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_invoke_nonexistent(void)
{
    printf("  test_invoke_nonexistent...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *out_output = NULL;
    int ret = agent_service_invoke(svc, "nonexistent_agent_id", "hi", 2, NULL, &out_output);
    assert(ret == AIRY_ERR_NOT_FOUND);
    assert(out_output != NULL);
    assert(strstr(out_output, "Agent not found") != NULL);
    AIRY_FREE(out_output);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_terminate_nonexistent(void)
{
    printf("  test_terminate_nonexistent...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    int ret = agent_service_terminate(svc, "nonexistent_agent_id");
    assert(ret == AIRY_ERR_NOT_FOUND);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_capacity_limit(void)
{
    printf("  test_capacity_limit...\n");

    agent_service_t *svc = agent_service_create(2);
    assert(svc != NULL);

    char *r1 = NULL, *r2 = NULL, *r3 = NULL;
    int sprc1 = agent_service_spawn(svc, "{\"n\":1}", &r1);
    assert(sprc1 == AIRY_SUCCESS);
    int sprc2 = agent_service_spawn(svc, "{\"n\":2}", &r2);
    assert(sprc2 == AIRY_SUCCESS);

    int ret = agent_service_spawn(svc, "{\"n\":3}", &r3);
    assert(ret != AIRY_SUCCESS);
    assert(r3 == NULL);

    AIRY_FREE(r1);
    AIRY_FREE(r2);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_spawn_after_terminate(void)
{
    printf("  test_spawn_after_terminate...\n");

    /* The implementation does not compact the array (terminate only sets
     * status=3), so max_agents=3 allows spawning again after termination:
     * spawn 2 (count=2), terminate 1 (count=2), spawn 1 (count=3) must
     * succeed */
    agent_service_t *svc = agent_service_create(3);
    assert(svc != NULL);

    char *r1 = NULL, *r2 = NULL;
    assert(agent_service_spawn(svc, "{\"n\":1}", &r1) == AIRY_SUCCESS);
    assert(agent_service_spawn(svc, "{\"n\":2}", &r2) == AIRY_SUCCESS);
    assert(agent_service_count(svc) == 2);

    assert(agent_service_terminate(svc, r1) == AIRY_SUCCESS);
    assert(agent_service_count(svc) == 2);

    char *r3 = NULL;
    int ret = agent_service_spawn(svc, "{\"n\":3}", &r3);
    assert(ret == AIRY_SUCCESS);
    assert(r3 != NULL);
    assert(agent_service_count(svc) == 3);

    char *out_output = NULL;
    int irc = agent_service_invoke(svc, r2, "hi", 2, NULL, &out_output);
    assert(irc != AIRY_SUCCESS);
    AIRY_FREE(out_output);

    AIRY_FREE(r1);
    AIRY_FREE(r2);
    AIRY_FREE(r3);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void *invoke_cancel_thread(void *arg)
{
    airy_sleep_ms(150);
    airy_cancel_token_cancel((airy_cancel_token_t *)arg);
    return NULL;
}

static void test_invoke_cancel(void)
{
    printf("  test_invoke_cancel...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, "{\"type\":\"block\"}", &agent_id);
    assert(ret == AIRY_SUCCESS && agent_id != NULL);

    int in_pipe[2], out_pipe[2];
    assert(pipe(in_pipe) == 0 && pipe(out_pipe) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        setpgid(0, 0);
        char buf[64];
        (void)read(STDIN_FILENO, buf, sizeof(buf));
        sleep(30);
        _exit(0);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);

    agent_entry_internal_t *agent = NULL;
    for (size_t i = 0; i < svc->agent_count; i++) {
        if (strcmp(svc->agents[i].agent_id, agent_id) == 0) {
            agent = &svc->agents[i];
            break;
        }
    }
    assert(agent != NULL);
    agent->child_pid = pid;
    agent->stdin_fd = in_pipe[1];
    agent->stdout_fd = out_pipe[0];
    agent->status = AGENT_STATUS_RUNNING;
    agent->last_active = (uint64_t)time(NULL);

    airy_cancel_token_t token;
    assert(airy_cancel_token_init(&token) == 0);
    airy_thread_t th;
    assert(airy_platform_thread_create(&th, invoke_cancel_thread, &token) == 0);

    char *out_output = NULL;
    ret = agent_service_invoke(svc, agent_id, "ping", 4, &token, &out_output);
    assert(airy_platform_thread_join(th, NULL) == 0);

    assert(ret == AIRY_ERR_CANCELED);
    assert(out_output != NULL);
    assert(strstr(out_output, "aborted") != NULL);
    assert(agent->child_pid <= 0);

    AIRY_FREE(out_output);
    AIRY_FREE(agent_id);
    airy_cancel_token_destroy(&token);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void *session_cancel_thread(void *arg)
{
    agent_service_t *svc = (agent_service_t *)arg;
    airy_sleep_ms(150);
    int rc = agent_service_invoke_cancel(svc, "req-session-1");
    assert(rc == AIRY_SUCCESS);
    return NULL;
}

static void test_invoke_session_cancel(void)
{
    printf("  test_invoke_session_cancel...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, "{\"type\":\"block\"}", &agent_id);
    assert(ret == AIRY_SUCCESS && agent_id != NULL);

    int in_pipe[2], out_pipe[2];
    assert(pipe(in_pipe) == 0 && pipe(out_pipe) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        setpgid(0, 0);
        char buf[64];
        (void)read(STDIN_FILENO, buf, sizeof(buf));
        sleep(30);
        _exit(0);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);

    agent_entry_internal_t *agent = NULL;
    for (size_t i = 0; i < svc->agent_count; i++) {
        if (strcmp(svc->agents[i].agent_id, agent_id) == 0) {
            agent = &svc->agents[i];
            break;
        }
    }
    assert(agent != NULL);
    agent->child_pid = pid;
    agent->stdin_fd = in_pipe[1];
    agent->stdout_fd = out_pipe[0];
    agent->status = AGENT_STATUS_RUNNING;
    agent->last_active = (uint64_t)time(NULL);

    airy_cancel_token_t *token = NULL;
    ret = agent_service_invoke_begin(svc, "req-session-1", &token);
    assert(ret == AIRY_SUCCESS && token != NULL);

    airy_thread_t th;
    assert(airy_platform_thread_create(&th, session_cancel_thread, svc) == 0);

    char *out_output = NULL;
    ret = agent_service_invoke(svc, agent_id, "ping", 4, token, &out_output);
    assert(airy_platform_thread_join(th, NULL) == 0);

    assert(ret == AIRY_ERR_CANCELED);
    assert(out_output != NULL);
    assert(strstr(out_output, "aborted") != NULL);

    agent_service_invoke_end(svc, "req-session-1");
    agent_service_invoke_end(svc, "req-session-1");

    assert(agent_service_invoke_cancel(svc, "req-session-1") == AIRY_ERR_NOT_FOUND);

    AIRY_FREE(out_output);
    AIRY_FREE(agent_id);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_invoke_session_capacity(void)
{
    printf("  test_invoke_session_capacity...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    airy_cancel_token_t *tokens[AGENT_INVOKE_SESSIONS_MAX];
    char rid[64];
    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        snprintf(rid, sizeof(rid), "req-cap-%zu", i);
        int rc = agent_service_invoke_begin(svc, rid, &tokens[i]);
        assert(rc == AIRY_SUCCESS && tokens[i] != NULL);
    }

    airy_cancel_token_t *overflow = NULL;
    int rc = agent_service_invoke_begin(svc, "req-cap-overflow", &overflow);
    assert(rc == AIRY_ERR_BUSY && overflow == NULL);

    agent_service_invoke_end(svc, "req-cap-0");
    airy_cancel_token_t *t2 = NULL;
    rc = agent_service_invoke_begin(svc, "req-cap-overflow", &t2);
    assert(rc == AIRY_SUCCESS && t2 != NULL);
    agent_service_invoke_end(svc, "req-cap-overflow");

    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        snprintf(rid, sizeof(rid), "req-cap-%zu", i);
        agent_service_invoke_end(svc, rid);
    }

    agent_service_destroy(svc);
    printf("    PASSED\n");
}

/* ========== main ========== */
int main(void)
{
    /* P0-2: unit tests use the deterministic mode — real Python child
     * processes must not be forked; verifies the service state machine and
     * the new "no-child invoke returns a clear error" contract. */
    setenv("AIRY_AGENT_NO_SPAWN", "1", 1);

    printf("=== Agent Service Unit Tests ===\n");
    test_create_destroy();
    test_spawn_and_list();
    test_terminate();
    test_invoke();
    test_invoke_nonexistent();
    test_terminate_nonexistent();
    test_capacity_limit();
    test_spawn_after_terminate();

    test_invoke_cancel();

    test_invoke_session_cancel();
    test_invoke_session_capacity();
    printf("=== All tests PASSED ===\n");
    return 0;
}
