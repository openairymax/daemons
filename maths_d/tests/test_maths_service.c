/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file test_maths_service.c
 * @brief 数学外挂计算服务单元测试。
 */

#include "maths_service.h"
#include "airy_memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK_NEAR(expr, expected, tol)                                        \
    do {                                                                       \
        double _got = (expr);                                                  \
        double _exp = (expected);                                              \
        if (fabs(_got - _exp) > (tol)) {                                       \
            printf("FAIL %s:%d: %s = %.9g, expected %.9g\n", __FILE__,         \
                   __LINE__, #expr, _got, _exp);                               \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_OK(expr)                                                         \
    do {                                                                       \
        double _r = 0.0;                                                       \
        char _e[128] = "";                                                     \
        if (maths_d_eval((expr), &_r, _e, sizeof(_e)) != 0) {                  \
            printf("FAIL %s:%d: eval(%s) failed: %s\n", __FILE__, __LINE__,    \
                   (expr), _e);                                                \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_ERR(expr)                                                        \
    do {                                                                       \
        double _r = 0.0;                                                       \
        char _e[128] = "";                                                     \
        if (maths_d_eval((expr), &_r, _e, sizeof(_e)) == 0) {                  \
            printf("FAIL %s:%d: eval(%s) should have failed\n", __FILE__,      \
                   __LINE__, (expr));                                          \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_RESULT(expr, expected)                                           \
    do {                                                                       \
        double _r = 0.0;                                                       \
        char _e[128] = "";                                                     \
        if (maths_d_eval((expr), &_r, _e, sizeof(_e)) != 0) {                  \
            printf("FAIL %s:%d: eval(%s) failed: %s\n", __FILE__, __LINE__,    \
                   (expr), _e);                                                \
            g_fail++;                                                          \
        } else {                                                               \
            CHECK_NEAR(_r, (expected), 1e-9);                                  \
        }                                                                      \
    } while (0)

static void test_basic_arithmetic(void)
{
    CHECK_RESULT("1+1", 2.0);
    CHECK_RESULT("125*38/7.2+15", 125.0 * 38.0 / 7.2 + 15.0);
    CHECK_RESULT("2^10", 1024.0);
    CHECK_RESULT("sqrt(144)", 12.0);
    CHECK_RESULT("17%5", 2.0);
    CHECK_RESULT("abs(-5)", 5.0);
    CHECK_RESULT("-3+5", 2.0);
    CHECK_RESULT("2*(3+4)", 14.0);
    CHECK_RESULT("2^3^2", 512.0); /* 右结合：2^(3^2) */
    CHECK_RESULT("5!", 120.0);
    CHECK_RESULT("pi", 3.14159265358979323846);
    CHECK_RESULT("2*pi", 2.0 * 3.14159265358979323846);
}

static void test_functions(void)
{
    CHECK_RESULT("sin(pi/6)", 0.5);
    CHECK_RESULT("cos(0)", 1.0);
    CHECK_RESULT("log(8,2)", 3.0);
    CHECK_RESULT("ln(exp(3))", 3.0);
    CHECK_RESULT("exp(0)", 1.0);
    CHECK_RESULT("floor(3.7)", 3.0);
    CHECK_RESULT("ceil(3.2)", 4.0);
    CHECK_RESULT("round(3.5)", 4.0);
    CHECK_RESULT("pow(3,3)", 27.0);
    CHECK_RESULT("min(3,7)", 3.0);
    CHECK_RESULT("max(3,7)", 7.0);
    CHECK_RESULT("log10(1000)", 3.0);
    CHECK_RESULT("factorial(10)", 3628800.0);
    CHECK_RESULT("2+3*4^2", 2.0 + 3.0 * 16.0);
}

static void test_error_cases(void)
{
    CHECK_ERR("1/0");
    CHECK_ERR("5%0");
    CHECK_ERR("(1+2");
    CHECK_ERR("1+2)");
    CHECK_ERR("foo(3)");
    CHECK_ERR("3 +");
    CHECK_ERR("factorial(2.5)");
    CHECK_ERR("factorial(-1)");
    CHECK_ERR("x+y");
    CHECK_ERR("");
}

static void test_stats(void)
{
    double v[] = { 1.0, 2.0, 3.0, 4.0, 5.0 };
    char err[128] = "";
    double r = 0.0;

    if (maths_d_stats("mean", v, 5, &r, err, sizeof(err)) != 0) {
        printf("FAIL mean\n");
        g_fail++;
    }
    CHECK_NEAR(r, 3.0, 1e-9);

    if (maths_d_stats("median", v, 5, &r, err, sizeof(err)) != 0) {
        printf("FAIL median\n");
        g_fail++;
    }
    CHECK_NEAR(r, 3.0, 1e-9);

    if (maths_d_stats("sum", v, 5, &r, err, sizeof(err)) != 0) {
        printf("FAIL sum\n");
        g_fail++;
    }
    CHECK_NEAR(r, 15.0, 1e-9);

    if (maths_d_stats("max", v, 5, &r, err, sizeof(err)) != 0) {
        printf("FAIL max\n");
        g_fail++;
    }
    CHECK_NEAR(r, 5.0, 1e-9);

    if (maths_d_stats("stddev", v, 5, &r, err, sizeof(err)) != 0) {
        printf("FAIL stddev\n");
        g_fail++;
    }
    CHECK_NEAR(r, sqrt(2.0), 1e-9); /* 总体标准差 = sqrt(2) */

    if (maths_d_stats("bogus", v, 5, &r, err, sizeof(err)) == 0) {
        printf("FAIL bogus op should error\n");
        g_fail++;
    }
}

static void test_recognize(void)
{
    if (!maths_d_recognize("125*38/7.2+15")) {
        printf("FAIL recognize arithmetic\n");
        g_fail++;
    }
    if (!maths_d_recognize("sqrt(144)")) {
        printf("FAIL recognize function\n");
        g_fail++;
    }
    if (!maths_d_recognize("42")) {
        printf("FAIL recognize bare number\n");
        g_fail++;
    }
    if (maths_d_recognize("帮我写一首诗")) {
        printf("FAIL recognize chinese text\n");
        g_fail++;
    }
    if (maths_d_recognize("Hello world")) {
        printf("FAIL recognize plain english\n");
        g_fail++;
    }
}

/* 数学后端方法路由：numerical/finance/number_theory 与符号方法必须被识别
 * 为后端方法（backend 不可用时返回 backend 错误，而非 "method not found"）。 */
static void test_backend_method_routing(void)
{
    maths_d_service_t svc;
    AIRY_MEMSET(&svc, 0, sizeof(svc));
    svc.py_backend.in_fd = -1;
    svc.py_backend.out_fd = -1;
    svc.py_backend.available = 0; /* 模拟未部署 maths-toolkit */

    const char *methods[] = {
        "solve", "differentiate", "integrate", "limit", "simplify",
        "factor", "expand", "matrix", "units", "numerical", "finance",
        "number_theory"
    };
    size_t i;
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        char req[256];
        char resp[2048];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":{}}",
                 methods[i]);
        if (maths_d_dispatch_jsonrpc(&svc, req, resp, sizeof(resp)) !=
            MATHS_METHOD_HANDLED) {
            printf("FAIL routing %s (not handled)\n", methods[i]);
            g_fail++;
            continue;
        }
        /* backend 不可用时应返回 backend 错误，而非 method not found */
        if (strstr(resp, "method not found")) {
            printf("FAIL routing %s (fell through to method not found)\n",
                   methods[i]);
            g_fail++;
        }
        if (!strstr(resp, "backend unavailable")) {
            printf("FAIL routing %s (missing backend-unavailable error)\n",
                   methods[i]);
            g_fail++;
        }
    }

    /* 未知方法仍应返回 method not found */
    char req[256];
    char resp[2048];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bogus_math\",\"params\":{}}");
    if (maths_d_dispatch_jsonrpc(&svc, req, resp, sizeof(resp)) !=
        MATHS_METHOD_HANDLED ||
        !strstr(resp, "method not found")) {
        printf("FAIL unknown method should be method not found\n");
        g_fail++;
    }
}

int main(void)
{
    test_basic_arithmetic();
    test_functions();
    test_error_cases();
    test_stats();
    test_recognize();
    test_backend_method_routing();

    if (g_fail == 0) {
        printf("maths_service tests: ALL PASSED\n");
        return 0;
    }
    printf("maths_service tests: %d FAILED\n", g_fail);
    return 1;
}
