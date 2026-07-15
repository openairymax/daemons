// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/*
 * Copyright (c) 2026 SPHARX Ltd. All Rights Reserved.
 * test_checkpoint.c - Checkpoint Module Unit Tests
 */

#include "checkpoint.h"  /* SP02: migrated to commons/utils/execution/include/ */

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

#define TEST(name) do { tests_run++; printf("  %-50s", name); } while(0)
#define PASS() do { tests_passed++; printf("[PASS]\n"); } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); return; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); } } while(0)

static void setup_temp_dir(void)
{
    snprintf(g_storage_path, sizeof(g_storage_path), "/tmp/airy_checkpoint_test_%d", getpid());
    mkdir(g_storage_path, 0755);
    airy_checkpoint_init(g_storage_path);
}

static void teardown_temp_dir(void)
{
    airy_checkpoint_shutdown();
    /* P2-25: 使用 fork/exec 替代 system() 防止 shell 注入 */
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", g_storage_path, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int wstatus;
        while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}
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

static void test_create_and_verify(void)
{
    TEST("Create and verify checkpoint");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create(
        "task_001", "session_001", 1, "{\"key\":\"value\"}",
        NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create should succeed");
    ASSERT(cp != NULL, "checkpoint should not be NULL");
    ASSERT(strcmp(cp->task_id, "task_001") == 0, "task_id should match");
    ASSERT(cp->sequence_num == 1, "sequence_num should be 1");

    bool valid = false;
    err = airy_checkpoint_verify(cp, &valid);
    ASSERT(err == AIRY_OK, "verify should succeed");
    ASSERT(valid == true, "checkpoint should be valid");

    airy_checkpoint_destroy(cp);
    teardown_temp_dir();
    PASS();
}

static void test_create_null_params(void)
{
    TEST("Create with null params rejected");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create(
        NULL, "session", 1, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err != AIRY_OK, "null task_id should be rejected");

    err = airy_checkpoint_create(
        "task", NULL, 1, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "null session_id should be allowed (optional)");
    airy_checkpoint_destroy(cp);
    cp = NULL;

    err = airy_checkpoint_create(
        "task", "session", 1, NULL, NULL, 0, NULL, 0, &cp);
    ASSERT(err != AIRY_OK, "null state_json should be rejected");

    err = airy_checkpoint_create(
        "task", "session", 1, "{}", NULL, 0, NULL, 0, NULL);
    ASSERT(err != AIRY_OK, "null out_checkpoint should be rejected");

    teardown_temp_dir();
    PASS();
}

static void test_save_and_restore(void)
{
    TEST("Save and restore checkpoint");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create(
        "task_save", "session_001", 1, "{\"state\":\"saved\"}",
        NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create should succeed");

    err = airy_checkpoint_save(cp);
    ASSERT(err == AIRY_OK, "save should succeed");

    airy_task_checkpoint_t *restored = NULL;
    err = airy_checkpoint_restore("task_save", 1, &restored);
    ASSERT(err == AIRY_OK, "restore should succeed");
    ASSERT(restored != NULL, "restored should not be NULL");
    ASSERT(strcmp(restored->task_id, "task_save") == 0, "restored task_id should match");
    ASSERT(restored->sequence_num == 1, "restored sequence_num should match");

    airy_checkpoint_destroy(cp);
    airy_checkpoint_destroy(restored);
    teardown_temp_dir();
    PASS();
}

static void test_restore_nonexistent(void)
{
    TEST("Restore nonexistent checkpoint");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_restore("nonexistent", 999, &cp);
    ASSERT(err != AIRY_OK, "restore nonexistent should fail");

    teardown_temp_dir();
    PASS();
}

static void test_list_checkpoints(void)
{
    TEST("List checkpoints for task");
    setup_temp_dir();

    airy_task_checkpoint_t *cp1 = NULL;
    airy_err_t err = airy_checkpoint_create(
        "task_list", "session_001", 1, "{}", NULL, 0, NULL, 0, &cp1);
    ASSERT(err == AIRY_OK, "create cp1");
    airy_checkpoint_save(cp1);
    airy_checkpoint_destroy(cp1);

    airy_task_checkpoint_t *cp2 = NULL;
    err = airy_checkpoint_create(
        "task_list", "session_001", 2, "{}", NULL, 0, NULL, 0, &cp2);
    ASSERT(err == AIRY_OK, "create cp2");
    airy_checkpoint_save(cp2);
    airy_checkpoint_destroy(cp2);

    airy_task_checkpoint_t **list = NULL;
    size_t count = 0;
    err = airy_checkpoint_list("task_list", &list, &count);
    ASSERT(err == AIRY_OK, "list should succeed");
    /* v0.1.1: 文件名含 sequence_num，seq=1 与 seq=2 各自独立落盘，
     * 不再相互覆盖，故 list 返回 2 个检查点。 */
    ASSERT(count == 2, "should have 2 checkpoints (one per sequence)");

    for (size_t i = 0; i < count; i++) {
        airy_checkpoint_destroy(list[i]);
    }
    free(list);

    teardown_temp_dir();
    PASS();
}

static void test_delete(void)
{
    TEST("Delete checkpoint");
    setup_temp_dir();

    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create(
        "task_del", "session_001", 1, "{}", NULL, 0, NULL, 0, &cp);
    ASSERT(err == AIRY_OK, "create");
    airy_checkpoint_save(cp);
    airy_checkpoint_destroy(cp);

    err = airy_checkpoint_delete("task_del", 1);
    ASSERT(err == AIRY_OK, "delete should succeed");

    airy_task_checkpoint_t *restored = NULL;
    err = airy_checkpoint_restore("task_del", 1, &restored);
    ASSERT(err != AIRY_OK, "restore after delete should fail");

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

static void test_verify_null(void)
{
    TEST("Verify null checkpoint");
    bool valid = true;
    airy_err_t err = airy_checkpoint_verify(NULL, &valid);
    ASSERT(err != AIRY_OK, "verify null should fail");
    PASS();
}

static void test_create_with_completed_nodes(void)
{
    TEST("Create with completed nodes");
    setup_temp_dir();

    char *completed[] = {"node_a", "node_b", "node_c"};
    airy_task_checkpoint_t *cp = NULL;
    airy_err_t err = airy_checkpoint_create(
        "task_nodes", "session_001", 1, "{}",
        completed, 3, NULL, 0, &cp);
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
    airy_err_t err = airy_checkpoint_create(
        "task_pending", "session_001", 1, "{}",
        NULL, 0, pending, 2, &cp);
    ASSERT(err == AIRY_OK, "create with pending nodes should succeed");
    ASSERT(cp->pending_count == 2, "should have 2 pending nodes");

    airy_checkpoint_destroy(cp);
    teardown_temp_dir();
    PASS();
}

static void test_multiple_tasks(void)
{
    TEST("Multiple tasks with checkpoints");
    setup_temp_dir();

    airy_task_checkpoint_t *cp1 = NULL, *cp2 = NULL;
    airy_checkpoint_create("task_A", "session_001", 1, "{}", NULL, 0, NULL, 0, &cp1);
    airy_checkpoint_save(cp1);
    airy_checkpoint_destroy(cp1);

    airy_checkpoint_create("task_B", "session_001", 1, "{}", NULL, 0, NULL, 0, &cp2);
    airy_checkpoint_save(cp2);
    airy_checkpoint_destroy(cp2);

    airy_task_checkpoint_t **list = NULL;
    size_t count = 0;
    airy_err_t err = airy_checkpoint_list("task_A", &list, &count);
    ASSERT(err == AIRY_OK, "list task_A");
    ASSERT(count == 1, "task_A should have 1 checkpoint");
    for (size_t i = 0; i < count; i++)
        airy_checkpoint_destroy(list[i]);
    free(list);

    list = NULL;
    count = 0;
    err = airy_checkpoint_list("task_B", &list, &count);
    ASSERT(err == AIRY_OK, "list task_B");
    ASSERT(count == 1, "task_B should have 1 checkpoint");
    for (size_t i = 0; i < count; i++)
        airy_checkpoint_destroy(list[i]);
    free(list);

    teardown_temp_dir();
    PASS();
}

int main(void)
{
    printf("\n=== Checkpoint Module Unit Tests ===\n\n");

    test_init_shutdown();
    test_create_and_verify();
    test_create_null_params();
    test_save_and_restore();
    test_restore_nonexistent();
    test_list_checkpoints();
    test_delete();
    test_get_stats();
    test_destroy_null();
    test_verify_null();
    test_create_with_completed_nodes();
    test_create_with_pending_nodes();
    test_multiple_tasks();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}