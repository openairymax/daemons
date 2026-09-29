/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file test_maths_service.c
 * @brief 数学外挂计算服务单元测试（域层 maths_d_rpc_call 直测）。
 */

#include "maths_service.h"
#include "expr_eval.h"

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

/* 构造未部署 Python 后端的服务实例（锁经正式 init 初始化） */
static int test_svc_init(maths_d_service_t *svc)
{
    if (maths_d_service_init(svc) != 0) {
        printf("FAIL service init\n");
        g_fail++;
        return -1;
    }
    svc->py_backend.in_fd = -1;
    svc->py_backend.out_fd = -1;
    svc->py_backend.available = 0;
    return 0;
}

static void test_svc_teardown(maths_d_service_t *svc)
{
    maths_backend_destroy(&svc->py_backend);
    maths_d_service_destroy(svc);
}

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

/* 后端方法路由：12 个符号/数值方法必须进入后端转发路径（后端不可用
 * 时返回域错误，而非 ERR_METHOD）；未知方法返回 ERR_METHOD。 */
static void test_backend_method_routing(void)
{
    maths_d_service_t svc;
    const char *methods[] = {
        "solve", "differentiate", "integrate", "limit", "simplify",
        "factor", "expand", "matrix", "units", "numerical", "finance",
        "number_theory"
    };
    size_t i;

    if (test_svc_init(&svc) != 0)
        return;

    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        cJSON *params = cJSON_Parse("{}");
        cJSON *result = NULL;
        char err[256] = "";
        maths_rpc_status_t st =
            maths_d_rpc_call(&svc, methods[i], params, &result, err,
                             sizeof(err));
        if (st != MATHS_RPC_ERR_DOMAIN ||
            !strstr(err, "backend unavailable")) {
            printf("FAIL routing %s (st=%d err=%s)\n", methods[i], (int)st,
                   err);
            g_fail++;
        }
        cJSON_Delete(params);
        cJSON_Delete(result);
    }

    {
        cJSON *params = cJSON_Parse("{}");
        cJSON *result = NULL;
        char err[256] = "";
        if (maths_d_rpc_call(&svc, "bogus_math", params, &result, err,
                             sizeof(err)) != MATHS_RPC_ERR_METHOD) {
            printf("FAIL unknown method should be ERR_METHOD\n");
            g_fail++;
        }
        cJSON_Delete(params);
        cJSON_Delete(result);
    }
    test_svc_teardown(&svc);
}

/* 变量绑定求值（绘图采样基础）：绑定变量参与运算、未绑定标识符仍报错、
 * maths_d_eval 无绑定语义不变。 */
static void test_eval_at(void)
{
    double r = 0.0;
    char e[128] = "";

    if (maths_d_eval_at("x^2", "x", 3.0, &r, e, sizeof(e)) != 0 ||
        fabs(r - 9.0) > 1e-9) {
        printf("FAIL eval_at x^2@3: %s\n", e);
        g_fail++;
    }
    if (maths_d_eval_at("sin(x)+x", "x", 0.0, &r, e, sizeof(e)) != 0 ||
        fabs(r - 0.0) > 1e-9) {
        printf("FAIL eval_at sin(x)+x@0: %s\n", e);
        g_fail++;
    }
    if (maths_d_eval_at("2*pi*x", "x", 1.0, &r, e, sizeof(e)) != 0 ||
        fabs(r - 2.0 * 3.14159265358979323846) > 1e-9) {
        printf("FAIL eval_at 2*pi*x@1: %s\n", e);
        g_fail++;
    }
    if (maths_d_eval_at("y+1", "x", 1.0, &r, e, sizeof(e)) == 0) {
        printf("FAIL eval_at unbound identifier should error\n");
        g_fail++;
    }
    if (maths_d_eval("x+1", &r, e, sizeof(e)) == 0) {
        printf("FAIL eval unbound x should error\n");
        g_fail++;
    }
}

/* plot 域调用：成功返回序列化 result（调用方 cJSON_free）；
 * 失败返回 NULL，err 填可读原因。 */
static char *test_plot_call(maths_d_service_t *svc, const char *params_json,
                            char *err, size_t err_sz)
{
    cJSON *params = cJSON_Parse(params_json);
    cJSON *result = NULL;
    char *out = NULL;

    if (!params) {
        printf("FAIL plot params parse\n");
        g_fail++;
        return NULL;
    }
    if (maths_d_rpc_call(svc, "plot", params, &result, err, err_sz) ==
            MATHS_RPC_OK &&
        result) {
        out = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
    }
    cJSON_Delete(params);
    return out;
}

/* plot RPC：正常采样、奇点 null、非法域、缺省与上限夹取。 */
static void test_plot_rpc(void)
{
    maths_d_service_t svc;
    char err[256] = "";
    char *resp;

    if (test_svc_init(&svc) != 0)
        return;

    /* y=x^2 on [-2,2]，5 样本 → xs=[-2,-1,0,1,2] ys=[4,1,0,1,4] */
    resp = test_plot_call(&svc,
                          "{\"expr\":\"x^2\",\"xmin\":-2,\"xmax\":2,"
                          "\"samples\":5}",
                          err, sizeof(err));
    if (!resp || !strstr(resp, "\"xs\":[-2,-1,0,1,2]") ||
        !strstr(resp, "\"ys\":[4,1,0,1,4]")) {
        printf("FAIL plot y=x^2: %s\n", resp ? resp : err);
        g_fail++;
    }
    cJSON_free(resp);

    /* 域内奇点（1/x @ x=0）→ null */
    resp = test_plot_call(&svc,
                          "{\"expr\":\"1/x\",\"xmin\":-1,\"xmax\":1,"
                          "\"samples\":3}",
                          err, sizeof(err));
    if (!resp || !strstr(resp, "\"ys\":[-1,null,1]")) {
        printf("FAIL plot singular point should be null: %s\n",
               resp ? resp : err);
        g_fail++;
    }
    cJSON_free(resp);

    /* 非法域：xmin >= xmax → ERR_DOMAIN + 可读 err */
    resp = test_plot_call(&svc, "{\"expr\":\"x\",\"xmin\":2,\"xmax\":-2}",
                          err, sizeof(err));
    if (resp || err[0] == '\0') {
        printf("FAIL plot bad domain should error: %s\n",
               resp ? resp : "(no err)");
        g_fail++;
    }
    cJSON_free(resp);

    /* samples 缺省 → 128 */
    resp = test_plot_call(&svc, "{\"expr\":\"x\",\"xmin\":0,\"xmax\":1}",
                          err, sizeof(err));
    if (!resp || !strstr(resp, "\"samples\":128")) {
        printf("FAIL plot default samples: %s\n", resp ? resp : err);
        g_fail++;
    }
    cJSON_free(resp);

    /* 超限 → 夹取 256 */
    resp = test_plot_call(&svc,
                          "{\"expr\":\"x\",\"xmin\":0,\"xmax\":1,"
                          "\"samples\":99999}",
                          err, sizeof(err));
    if (!resp || !strstr(resp, "\"samples\":256")) {
        printf("FAIL plot clamp samples: %s\n", resp ? resp : err);
        g_fail++;
    }
    cJSON_free(resp);

    test_svc_teardown(&svc);
}

int main(void)
{
    test_basic_arithmetic();
    test_functions();
    test_error_cases();
    test_stats();
    test_recognize();
    test_backend_method_routing();
    test_eval_at();
    test_plot_rpc();

    if (g_fail == 0) {
        printf("maths_service tests: ALL PASSED\n");
        return 0;
    }
    printf("maths_service tests: %d FAILED\n", g_fail);
    return 1;
}
