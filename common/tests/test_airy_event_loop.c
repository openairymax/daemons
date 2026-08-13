// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_airy_event_loop.c - AgentRT Event Loop Module Unit Tests
 */

#include "../include/airy_event_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int tests_run = 0;
static int tests_passed = 0;

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

static int timer_fired_count = 0;

/* Cross-platform test fd: eventfd(2) is Linux-specific; a non-blocking pipe
 * read end is POSIX and works on macOS too.  The tests only need an fd that
 * can be watched by the event loop. */
static int test_make_fd(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    close(fds[1]);
    return fds[0];
}

static void timer_cb(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;
    (void)user_data;
    timer_fired_count++;
}

static int fd_cb(int fd, uint32_t events, void *user_data)
{
    (void)fd;
    (void)events;
    (void)user_data;
    return 0;
}

static void test_create_default(void)
{
    TEST("Create with default max_events");
    airy_event_loop_t *loop = airy_event_loop_create(AIRY_EVENT_LOOP_MAX_EVENTS);
    ASSERT(loop != NULL, "create should succeed");
    airy_event_loop_destroy(loop);
    PASS();
}

static void test_create_zero_events(void)
{
    TEST("Create with zero max_events");
    airy_event_loop_t *loop = airy_event_loop_create(0);
    ASSERT(loop != NULL || loop == NULL, "create with 0 should not crash");
    if (loop)
        airy_event_loop_destroy(loop);
    PASS();
}

static void test_destroy_null(void)
{
    TEST("Destroy NULL loop is safe");
    airy_event_loop_destroy(NULL);
    PASS();
}

static void test_add_fd(void)
{
    TEST("Add an fd to watch");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    int efd = test_make_fd();
    ASSERT(efd >= 0, "create eventfd");

    int ret = airy_event_loop_add_fd(loop, efd, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
    ASSERT(ret == 0, "add fd should succeed");

    int count = airy_event_loop_get_fd_count(loop);
    ASSERT(count >= 1, "fd count should be at least 1 after add");

    airy_event_loop_remove_fd(loop, efd);
    close(efd);
    airy_event_loop_destroy(loop);
    PASS();
}

static void test_add_fd_with_callback(void)
{
    TEST("Add fd with callback");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    int efd = test_make_fd();
    ASSERT(efd >= 0, "create eventfd");

    int ret = airy_event_loop_add_fd(loop, efd, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
    ASSERT(ret == 0, "add fd with callback should succeed");

    airy_event_loop_remove_fd(loop, efd);
    close(efd);
    airy_event_loop_destroy(loop);
    PASS();
}

static void test_add_fd_invalid(void)
{
    TEST("Add invalid fd rejected");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    int ret = airy_event_loop_add_fd(loop, -1, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
    ASSERT(ret != 0, "invalid fd should be rejected");

    airy_event_loop_destroy(loop);
    PASS();
}

static void test_add_fd_null_loop(void)
{
    TEST("Add fd to NULL loop rejected");
    int efd = test_make_fd();
    int ret = airy_event_loop_add_fd(NULL, efd, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
    ASSERT(ret != 0, "NULL loop should be rejected");
    close(efd);
    PASS();
}

static void test_add_fd_null_callback(void)
{
    TEST("Add fd with NULL callback rejected");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    int efd = test_make_fd();
    ASSERT(efd >= 0, "create eventfd");

    int ret = airy_event_loop_add_fd(loop, efd, AIRY_EVENT_TYPE_READ, NULL, NULL);
    ASSERT(ret != 0, "NULL callback should be rejected");

    close(efd);
    airy_event_loop_destroy(loop);
    PASS();
}

static void test_remove_fd(void)
{
    TEST("Remove fd from watch");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    int efd = test_make_fd();
    ASSERT(efd >= 0, "create eventfd");

    airy_event_loop_add_fd(loop, efd, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
    int pre_count = airy_event_loop_get_fd_count(loop);

    airy_event_loop_remove_fd(loop, efd);

    int post_count = airy_event_loop_get_fd_count(loop);
    ASSERT(post_count < pre_count, "fd count should decrease after remove");

    close(efd);
    airy_event_loop_destroy(loop);
    PASS();
}

static void test_remove_fd_null_loop(void)
{
    TEST("Remove fd from NULL loop is safe");
    airy_event_loop_remove_fd(NULL, 0);
    PASS();
}

static void test_get_fd_count(void)
{
    TEST("Get fd count tracking");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    int efd1 = test_make_fd();
    int efd2 = test_make_fd();
    ASSERT(efd1 >= 0 && efd2 >= 0, "create eventfds");

    int ret1 = airy_event_loop_add_fd(loop, efd1, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
    int ret2 = airy_event_loop_add_fd(loop, efd2, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
    ASSERT(ret1 == 0 && ret2 == 0, "add fds");

    int count = airy_event_loop_get_fd_count(loop);
    ASSERT(count == 2, "fd count should be 2 after adding 2 fds");

    airy_event_loop_remove_fd(loop, efd1);
    count = airy_event_loop_get_fd_count(loop);
    ASSERT(count == 1, "fd count should be 1 after removing 1 fd");

    airy_event_loop_remove_fd(loop, efd2);
    count = airy_event_loop_get_fd_count(loop);
    ASSERT(count == 0, "fd count should be 0 after removing all fds");

    close(efd1);
    close(efd2);
    airy_event_loop_destroy(loop);
    PASS();
}

static void test_add_timer(void)
{
    TEST("Add timer");
    timer_fired_count = 0;
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    uint64_t timer_id = airy_event_loop_add_timer(loop, 100, timer_cb, NULL);
    ASSERT(timer_id > 0, "add timer should return valid id");

    airy_event_loop_cancel_timer(loop, timer_id);
    airy_event_loop_destroy(loop);
    PASS();
}

static void test_add_timer_null_callback(void)
{
    TEST("Add timer with NULL callback rejected");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    uint64_t timer_id = airy_event_loop_add_timer(loop, 100, NULL, NULL);
    ASSERT(timer_id == 0, "NULL callback should be rejected");

    airy_event_loop_destroy(loop);
    PASS();
}

static void test_add_timer_null_loop(void)
{
    TEST("Add timer to NULL loop rejected");
    uint64_t timer_id = airy_event_loop_add_timer(NULL, 100, timer_cb, NULL);
    ASSERT(timer_id == 0, "NULL loop should be rejected");
    PASS();
}

static void test_cancel_timer(void)
{
    TEST("Cancel timer by ID");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    uint64_t timer_id = airy_event_loop_add_timer(loop, 100, timer_cb, NULL);
    ASSERT(timer_id > 0, "add timer");

    int ret = airy_event_loop_cancel_timer(loop, timer_id);
    ASSERT(ret == 0, "cancel timer should succeed");

    airy_event_loop_destroy(loop);
    PASS();
}

static void test_cancel_timer_null_loop(void)
{
    TEST("Cancel timer on NULL loop rejected");
    int ret = airy_event_loop_cancel_timer(NULL, 1);
    ASSERT(ret != 0, "NULL loop should be rejected");
    PASS();
}

static void test_multiple_fds_remove(void)
{
    TEST("Multiple fds add then remove all");
    airy_event_loop_t *loop = airy_event_loop_create(512);
    ASSERT(loop != NULL, "create");

    int fds[5];
    for (int i = 0; i < 5; i++) {
        fds[i] = test_make_fd();
        ASSERT(fds[i] >= 0, "create eventfd");
        int ret = airy_event_loop_add_fd(loop, fds[i], AIRY_EVENT_TYPE_READ, fd_cb, NULL);
        ASSERT(ret == 0, "add fd");
    }

    int count = airy_event_loop_get_fd_count(loop);
    ASSERT(count == 5, "fd count should be 5");

    for (int i = 0; i < 5; i++) {
        airy_event_loop_remove_fd(loop, fds[i]);
        close(fds[i]);
    }

    count = airy_event_loop_get_fd_count(loop);
    ASSERT(count == 0, "fd count should be 0");

    airy_event_loop_destroy(loop);
    PASS();
}

static void test_multiple_cycles(void)
{
    TEST("Multiple create/destroy cycles");
    for (int i = 0; i < 5; i++) {
        airy_event_loop_t *loop = airy_event_loop_create(64);
        ASSERT(loop != NULL, "repeat create");

        int efd = test_make_fd();
        airy_event_loop_add_fd(loop, efd, AIRY_EVENT_TYPE_READ, fd_cb, NULL);
        uint64_t tid = airy_event_loop_add_timer(loop, 10, timer_cb, NULL);

        airy_event_loop_remove_fd(loop, efd);
        close(efd);
        if (tid > 0) {
            airy_event_loop_cancel_timer(loop, tid);
        }

        airy_event_loop_destroy(loop);
    }
    PASS();
}

int main(void)
{
    printf("\n=== AgentRT Event Loop Module Unit Tests ===\n\n");

    test_create_default();
    test_create_zero_events();
    test_destroy_null();
    test_add_fd();
    test_add_fd_with_callback();
    test_add_fd_invalid();
    test_add_fd_null_loop();
    test_add_fd_null_callback();
    test_remove_fd();
    test_remove_fd_null_loop();
    test_get_fd_count();
    test_add_timer();
    test_add_timer_null_callback();
    test_add_timer_null_loop();
    test_cancel_timer();
    test_cancel_timer_null_loop();
    test_multiple_fds_remove();
    test_multiple_cycles();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}