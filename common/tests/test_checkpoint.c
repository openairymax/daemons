// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_checkpoint.c - Checkpoint Module Unit Tests
 */

#include "checkpoint.h" /* SP02: migrated to commons/utils/execution/ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static int tests_run = 0;
static int tests_passed = 0;
static char g_storage_path[256] = {0};

#define TEST(name)               \
    do {                         \
        tests_run++;             \
        printf("  %-50s", name); \
    } while (0)
#define PASS()              \
    do {                    \
        tests_passed++;     \
        printf("[PASS]\n"); \
    } while (0)
#define FAIL(msg)                   \
    do {                            \
        printf("[FAIL] %s\n", msg); \
        return;                     \
    } while (0)
#define ASSERT(cond, msg) \
    do {                  \
        if (!(cond)) {    \
            FAIL(msg);    \
        }                 \
    } while (0)

static void setup_temp_dir(void)
{
    snprintf(g_storage_path, sizeof(g_storage_path), "/tmp/airy_checkpoint_test_%d", getpid());
    mkdir(g_storage_path, 0755);
    airy_checkpoint_init(g_storage_path);
}

static void teardown_temp_dir(void)
{
    airy_checkpoint_shutdown();

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", g_storage_path, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int wstatus;
        while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
        }
    }
}

static void test_init_shutdown(void)
{
    TEST("Init and shutdown");
    setup_temp_dir();
    airy_checkpoint_stats_t stats;
    airy_err_t err = airy_checkpoint_get_stats(&stats);
    ASSERT(err == AIRY_OK, "get_stats should succeed after init");
    teardown_temp_dir();
    PASS();
}

static void test_create_fields(void)
{
    TEST("Create checkpoint fields");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create("task_001", "session_001", 1, "{\"key\":\"value\"}",
                                            NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create should succeed");
    ASSERT(cp != NULL, "checkpoint should not be NULL");
    ASSERT(strcmp(cp->task_id, "task_001") == 0, "task_id should match");
    ASSERT(cp->sequence_num == 1, "sequence_num should be 1");
    ASSERT(cp->state_json != NULL, "state_json should be copied");
    ASSERT(cp->state == CHECKPOINT_STATE_PENDING, "state should start PENDING");

    airy_checkpoint_destroy(cp);
    teardown_temp_dir();
    PASS();
}

static void test_create_null_params(void)
{
    TEST("Create with null params rejected");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create(NULL, "session", 1, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err != AIRY_OK, "null task_id should be rejected");

    err = airy_checkpoint_create("task", NULL, 1, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "null session_id should be allowed (optional)");
    airy_checkpoint_destroy(cp);
    cp = NULL;

    err = airy_checkpoint_create("task", "session", 1, NULL, NULL, 0, NULL, 0, &cp);
    ASSERT(err != AIRY_OK, "null state_json should be rejected");

    err = airy_checkpoint_create("task", "session", 1, "{}", NULL, 0, NULL, 0, NULL);
    ASSERT(err != AIRY_OK, "null out_checkpoint should be rejected");

    teardown_temp_dir();
    PASS();
}

static void test_save_updates_stats(void)
{
    TEST("Save updates stats and state");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create("task_save", "session_001", 1, "{\"state\":\"saved\"}",
                                            NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create should succeed");

    err = airy_checkpoint_save(cp);
    ASSERT(err == AIRY_OK, "save should succeed");
    ASSERT(cp->state == CHECKPOINT_STATE_COMPLETED, "state should become COMPLETED");

    airy_checkpoint_stats_t stats;
    err = airy_checkpoint_get_stats(&stats);
    ASSERT(err == AIRY_OK, "get_stats should succeed");
    ASSERT(stats.total_checkpoints == 1, "total_checkpoints should be 1");
    ASSERT(stats.successful_checkpoints == 1, "successful_checkpoints should be 1");

    airy_checkpoint_destroy(cp);
    teardown_temp_dir();
    PASS();
}

static void test_list_task_ids(void)
{
    TEST("List distinct task ids");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create("task_a", "s", 1, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create a1");
    airy_checkpoint_save(cp);
    airy_checkpoint_destroy(cp);

    err = airy_checkpoint_create("task_a", "s", 2, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create a2");
    airy_checkpoint_save(cp);
    airy_checkpoint_destroy(cp);

    err = airy_checkpoint_create("task_b", "s", 1, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create b1");
    airy_checkpoint_save(cp);
    airy_checkpoint_destroy(cp);

    char **ids = NULL;
    size_t count = 0;
    err = airy_checkpoint_ids(&ids, &count);
    ASSERT(err == AIRY_OK, "ids should succeed");
    /* task_a 有两个 seq 必须去重为 1，连同 task_b 共 2 个 distinct id。
     * 这覆盖旧实现把 "{task_id}_{seq}" 整段当 id 导致去重失效的缺陷。 */
    ASSERT(count == 2, "should have 2 distinct task ids");

    int has_a = 0, has_b = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(ids[i], "task_a") == 0)
            has_a = 1;
        else if (strcmp(ids[i], "task_b") == 0)
            has_b = 1;
        free(ids[i]);
    }
    free(ids);
    ASSERT(has_a && has_b, "both task_a and task_b should be listed");

    teardown_temp_dir();
    PASS();
}

static void test_task_ids_empty(void)
{
    TEST("List task ids on empty dir");
    setup_temp_dir();

    char **ids = (char **)0x1;
    size_t count = 99;
    airy_err_t err = airy_checkpoint_ids(&ids, &count);
    ASSERT(err == AIRY_OK, "ids on empty dir should succeed");
    ASSERT(ids == NULL, "ids should be NULL when empty");
    ASSERT(count == 0, "count should be 0 when empty");

    teardown_temp_dir();
    PASS();
}

static void test_get_stats(void)
{
    TEST("Get checkpoint statistics");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_checkpoint_create("task_stats", "session_001", 1, "{}", NULL, 0, NULL, 0, &cp);
    airy_checkpoint_save(cp);
    airy_checkpoint_destroy(cp);

    airy_checkpoint_stats_t stats;
    airy_err_t err = airy_checkpoint_get_stats(&stats);
    ASSERT(err == AIRY_OK, "get_stats should succeed");
    ASSERT(stats.total_checkpoints >= 1, "should have at least 1 checkpoint");

    teardown_temp_dir();
    PASS();
}

static void test_destroy_null(void)
{
    TEST("Destroy null checkpoint");
    airy_checkpoint_destroy(NULL);
    PASS();
}

static void test_create_with_completed_nodes(void)
{
    TEST("Create with completed nodes");
    setup_temp_dir();

    char *completed[] = {"node_a", "node_b", "node_c"};
    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err =
        airy_checkpoint_create("task_nodes", "session_001", 1, "{}", completed, 3, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create with completed nodes should succeed");
    ASSERT(cp->completed_count == 3, "should have 3 completed nodes");

    airy_checkpoint_destroy(cp);
    teardown_temp_dir();
    PASS();
}

static void test_create_with_pending_nodes(void)
{
    TEST("Create with pending nodes");
    setup_temp_dir();

    char *pending[] = {"node_x", "node_y"};
    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err =
        airy_checkpoint_create("task_pending", "session_001", 1, "{}", NULL, 0, pending, 2, &cp);
    ASSERT(err == AIRY_OK, "create with pending nodes should succeed");
    ASSERT(cp->pending_count == 2, "should have 2 pending nodes");

    airy_checkpoint_destroy(cp);
    teardown_temp_dir();
    PASS();
}

int main(void)
{
    printf("\n=== Checkpoint Module Unit Tests ===\n\n");

    test_init_shutdown();
    test_create_fields();
    test_create_null_params();
    test_save_updates_stats();
    test_list_task_ids();
    test_task_ids_empty();
    test_get_stats();
    test_destroy_null();
    test_create_with_completed_nodes();
    test_create_with_pending_nodes();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
