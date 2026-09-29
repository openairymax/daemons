/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file maths_service.c
 * @brief 数学外挂计算服务核心实现。
 *
 * 职责划分（n11 大文件模块化，原文件 1020 行拆分）：
 *   - 数值表达式求值（递归下降求值器）→ src/expr_eval.c（独立模块）
 *   - 本文件保留：统计引擎（第五级描述性统计）、模式识别、
 *     RPC 域分发（协议无关，cJSON 出入）与 Python 后端转发、
 *     服务生命周期。JSON-RPC 传输与响应装配在 src/svc.c（svc 层）。
 *
 * 线性代数/符号计算由上层路由到 maths-toolkit Python 后端（MCP-Mathematics
 * + sympy-mcp），本服务经 stdio JSON-RPC 委托。
 */

#include "maths_service.h"
#include "expr_eval.h"
#include "platform_misc.h"
#include "airy_memory.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ==================== 统计引擎 ==================== */

static int maths_stats_dispatch(const char *op, const double *v, size_t n,
                                double *out, char *err, size_t err_sz)
{
    if (!op || !v || n == 0 || !out || !err || err_sz == 0)
        return -1;

    if (strcmp(op, "sum") == 0) {
        double acc = 0.0;
        for (size_t i = 0; i < n; i++)
            acc += v[i];
        *out = acc;
        return 0;
    }
    if (strcmp(op, "mean") == 0 || strcmp(op, "avg") == 0) {
        double acc = 0.0;
        for (size_t i = 0; i < n; i++)
            acc += v[i];
        *out = acc / (double)n;
        return 0;
    }
    if (strcmp(op, "max") == 0) {
        double m = v[0];
        for (size_t i = 1; i < n; i++)
            if (v[i] > m)
                m = v[i];
        *out = m;
        return 0;
    }
    if (strcmp(op, "min") == 0) {
        double m = v[0];
        for (size_t i = 1; i < n; i++)
            if (v[i] < m)
                m = v[i];
        *out = m;
        return 0;
    }
    if (strcmp(op, "median") == 0) {
        double *tmp = (double *)AIRY_MALLOC(n * sizeof(double));
        if (!tmp) {
            snprintf(err, err_sz, "out of memory");
            return -1;
        }
        AIRY_MEMCPY(tmp, v, n * sizeof(double));
        /* 简单插入排序（n 通常很小；大数据量场景由上层分片） */
        for (size_t i = 1; i < n; i++) {
            double key = tmp[i];
            size_t j = i;
            while (j > 0 && tmp[j - 1] > key) {
                tmp[j] = tmp[j - 1];
                j--;
            }
            tmp[j] = key;
        }
        if (n % 2 == 1)
            *out = tmp[n / 2];
        else
            *out = (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
        AIRY_FREE(tmp);
        return 0;
    }
    if (strcmp(op, "variance") == 0 || strcmp(op, "var") == 0 ||
        strcmp(op, "stddev") == 0) {
        double acc = 0.0;
        for (size_t i = 0; i < n; i++)
            acc += v[i];
        double mean = acc / (double)n;
        double sq = 0.0;
        for (size_t i = 0; i < n; i++) {
            double d = v[i] - mean;
            sq += d * d;
        }
        double var = sq / (double)n; /* 总体方差 */
        *out = (strcmp(op, "stddev") == 0) ? sqrt(var) : var;
        return 0;
    }

    snprintf(err, err_sz, "unsupported stats op '%s'", op);
    return -1;
}

int maths_d_stats(const char *op, const double *values, size_t count,
                  double *out_result, char *err_msg, size_t err_msg_size)
{
    return maths_stats_dispatch(op, values, count, out_result, err_msg,
                                err_msg_size);
}

/* ==================== 模式识别 ==================== */

/* 文本是否包含数学表达式特征：数字+运算符/函数名/括号/常量。
 * 用于 gateway 在送 LLM 前决策（决策结果记录 decision_reason 供审计）。 */
int maths_d_recognize(const char *text)
{
    if (!text || !text[0])
        return 0;

    size_t len = strlen(text);
    if (len > MATHS_MAX_EXPR_LEN)
        return 0;

    int has_digit = 0;
    int has_math_marker = 0;

    /* 常见数学函数名 */
    static const char *const funcs[] = {
        "sqrt(", "sin(", "cos(", "tan(", "asin(", "acos(", "atan(", "exp(",
        "log(", "ln(", "abs(", "floor(", "ceil(", "round(", "pow(", "min(",
        "max(", "factorial(", "cbrt(", "sign(", "mod(", "atan2(",
    };

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (isdigit(c))
            has_digit = 1;
    }

    if (!has_digit)
        return 0;

    /* 纯数字（单个数值）也算简单表达式 */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (strchr("+-*/^%!()=,", (int)c)) {
            has_math_marker = 1;
            break;
        }
        if (c == ' ' || c == '\t')
            continue;
        if (strncmp(text + i, "pi", 2) == 0 || strncmp(text + i, "e^", 2) == 0) {
            has_math_marker = 1;
            break;
        }
    }
    if (!has_math_marker) {
        /* 纯数字（单个数值，如 "42" / "3.14"）也视为简单表达式 */
        int all_numeric = 1;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)text[i];
            if (!(isdigit(c) || c == '.' || c == ' ' || c == '\t' ||
                  c == '-' || c == '+')) {
                all_numeric = 0;
                break;
            }
        }
        if (all_numeric)
            has_math_marker = 1;
    }
    if (!has_math_marker) {
        for (size_t f = 0; f < sizeof(funcs) / sizeof(funcs[0]); f++) {
            if (strstr(text, funcs[f])) {
                has_math_marker = 1;
                break;
            }
        }
    }

    return has_math_marker;
}

/* ==================== RPC 域分发 ==================== */

static const char *maths_req_str(const cJSON *params, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(params, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static cJSON *maths_health_result(maths_d_service_t *svc)
{
    airy_mtx_lock(&svc->lock);
    int healthy = atomic_load(&svc->running) ? 1 : 0;
    uint64_t evals = svc->eval_count;
    uint64_t errors = svc->error_count;
    uint64_t last_ms = svc->last_eval_ms;
    airy_mtx_unlock(&svc->lock);

    cJSON *r = cJSON_CreateObject();
    if (!r)
        return NULL;
    cJSON_AddStringToObject(r, "status", healthy ? "healthy" : "degraded");
    cJSON_AddStringToObject(r, "service", "maths_d");
    cJSON_AddNumberToObject(r, "eval_count", (double)evals);
    cJSON_AddNumberToObject(r, "error_count", (double)errors);
    cJSON_AddNumberToObject(r, "last_eval_ms", (double)last_ms);
    return r;
}

static cJSON *maths_usage_result(maths_d_service_t *svc)
{
    airy_mtx_lock(&svc->lock);
    uint64_t evals = svc->eval_count;
    uint64_t stats = svc->stats_count;
    uint64_t symbolic = svc->symbolic_count;
    uint64_t errors = svc->error_count;
    uint64_t last_ms = svc->last_eval_ms;
    uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
    int backend_up = maths_backend_available(&svc->py_backend);
    airy_mtx_unlock(&svc->lock);

    cJSON *r = cJSON_CreateObject();
    if (!r)
        return NULL;
    cJSON_AddStringToObject(r, "service", "maths_d");
    cJSON_AddNumberToObject(r, "eval_count", (double)evals);
    cJSON_AddNumberToObject(r, "stats_count", (double)stats);
    cJSON_AddNumberToObject(r, "symbolic_count", (double)symbolic);
    cJSON_AddStringToObject(r, "python_backend",
                            backend_up ? "up" : "degraded");
    cJSON_AddNumberToObject(r, "error_count", (double)errors);
    cJSON_AddNumberToObject(r, "last_eval_ms", (double)last_ms);
    cJSON_AddNumberToObject(r, "uptime_sec", (double)uptime);
    return r;
}

static cJSON *maths_eval_result(maths_d_service_t *svc, const cJSON *params,
                                char *err, size_t err_sz)
{
    const char *expr = maths_req_str(params, "expr");
    if (!expr) {
        snprintf(err, err_sz, "missing expr");
        return NULL;
    }

    double result = 0.0;
    char eval_err[128] = "";
    uint64_t t0 = airy_time_ms();
    int rc = maths_d_eval(expr, &result, eval_err, sizeof(eval_err));
    uint64_t elapsed_ms = airy_time_ms() - t0;

    airy_mtx_lock(&svc->lock);
    svc->last_eval_ms = elapsed_ms;
    if (rc == 0)
        svc->eval_count++;
    else
        svc->error_count++;
    airy_mtx_unlock(&svc->lock);

    if (rc != 0) {
        snprintf(err, err_sz, "%s", eval_err);
        return NULL;
    }

    cJSON *r = cJSON_CreateObject();
    if (!r)
        return NULL;
    cJSON_AddStringToObject(r, "expr", expr);
    cJSON_AddNumberToObject(r, "result", result);
    cJSON_AddNumberToObject(r, "elapsed_ms", (double)elapsed_ms);
    return r;
}

static cJSON *maths_stats_call(maths_d_service_t *svc, const cJSON *params,
                               char *err, size_t err_sz)
{
    const char *op = maths_req_str(params, "op");
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(params, "values");
    if (!op || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
        snprintf(err, err_sz, "invalid stats params (op + values[])");
        return NULL;
    }

    size_t count = (size_t)cJSON_GetArraySize(arr);
    if (count > MATHS_MAX_VALUES) {
        snprintf(err, err_sz, "too many values (max %d)", MATHS_MAX_VALUES);
        return NULL;
    }

    /* 堆分配：65536*8B=512KB 超线程栈安全预算，禁栈上大数组 */
    double *values = (double *)AIRY_MALLOC(count * sizeof(double));
    if (!values) {
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }

    size_t i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        if (!cJSON_IsNumber(it)) {
            AIRY_FREE(values);
            snprintf(err, err_sz, "values[] must be numbers");
            return NULL;
        }
        values[i++] = it->valuedouble;
    }

    double result = 0.0;
    char stat_err[128] = "";
    int rc = maths_d_stats(op, values, count, &result, stat_err,
                           sizeof(stat_err));
    AIRY_FREE(values);

    airy_mtx_lock(&svc->lock);
    if (rc == 0)
        svc->stats_count++;
    else
        svc->error_count++;
    airy_mtx_unlock(&svc->lock);

    if (rc != 0) {
        snprintf(err, err_sz, "%s", stat_err);
        return NULL;
    }

    cJSON *r = cJSON_CreateObject();
    if (!r)
        return NULL;
    cJSON_AddStringToObject(r, "op", op);
    cJSON_AddNumberToObject(r, "count", (double)count);
    cJSON_AddNumberToObject(r, "result", result);
    return r;
}

static cJSON *maths_plot_result(maths_d_service_t *svc, const cJSON *params,
                                char *err, size_t err_sz)
{
    const char *expr = maths_req_str(params, "expr");
    const cJSON *jmin = cJSON_GetObjectItemCaseSensitive(params, "xmin");
    const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(params, "xmax");
    if (!expr || !cJSON_IsNumber(jmin) || !cJSON_IsNumber(jmax) ||
        isnan(jmin->valuedouble) || isnan(jmax->valuedouble) ||
        !(jmin->valuedouble < jmax->valuedouble)) {
        snprintf(err, err_sz, "invalid plot params (expr, xmin<xmax)");
        return NULL;
    }

    const cJSON *jsamples =
        cJSON_GetObjectItemCaseSensitive(params, "samples");
    long n = cJSON_IsNumber(jsamples) ? (long)jsamples->valuedouble : 128;
    if (n < 2)
        n = 2;
    if (n > MATHS_PLOT_MAX_SAMPLES)
        n = MATHS_PLOT_MAX_SAMPLES;

    double xmin = jmin->valuedouble;
    double span = jmax->valuedouble - xmin;

    cJSON *r = cJSON_CreateObject();
    if (!r)
        return NULL;
    cJSON_AddStringToObject(r, "expr", expr);
    cJSON_AddNumberToObject(r, "xmin", xmin);
    cJSON_AddNumberToObject(r, "xmax", jmax->valuedouble);
    cJSON_AddNumberToObject(r, "samples", (double)n);
    cJSON *xs = cJSON_AddArrayToObject(r, "xs");
    cJSON *ys = xs ? cJSON_AddArrayToObject(r, "ys") : NULL;
    if (!xs || !ys) {
        cJSON_Delete(r);
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }

    /* 均匀采样 y=f(x)；x 域固定满采样，y 域错误/NaN/Inf 记 null，
     * 由渲染端断开连线。 */
    for (long i = 0; i < n; i++) {
        double x = xmin + span * (double)i / (double)(n - 1);
        double y = 0.0;
        char eval_err[128] = "";
        int ok = maths_d_eval_at(expr, "x", x, &y, eval_err,
                                 sizeof(eval_err)) == 0 &&
                 !isnan(y) && !isinf(y);
        cJSON *jx = cJSON_CreateNumber(x);
        cJSON *jy = ok ? cJSON_CreateNumber(y) : cJSON_CreateNull();
        if (!jx || !jy) {
            cJSON_Delete(jx);
            cJSON_Delete(jy);
            cJSON_Delete(r);
            snprintf(err, err_sz, "out of memory");
            return NULL;
        }
        cJSON_AddItemToArray(xs, jx);
        cJSON_AddItemToArray(ys, jy);
    }

    airy_mtx_lock(&svc->lock);
    svc->eval_count++;
    airy_mtx_unlock(&svc->lock);
    return r;
}

/* 12 个符号方法名：MCP-Mathematics（numerical/finance/number_theory/
 * units）+ SymPy 符号族（solve/differentiate/integrate/limit/simplify/
 * factor/expand/matrix），委托 Python 后端（maths-toolkit）。 */
static int maths_py_method(const char *m)
{
    static const char *const k_backend[] = {
        "solve",    "differentiate", "integrate", "limit",
        "simplify", "factor",        "expand",    "matrix",
        "units",    "numerical",     "finance",   "number_theory",
    };
    for (size_t i = 0; i < sizeof(k_backend) / sizeof(k_backend[0]); i++)
        if (strcmp(m, k_backend[i]) == 0)
            return 1;
    return 0;
}

/* 后端响应剥壳：{"result":{...}} 深转移 / {"error":{...}} 归一为
 * -32000+message（wire 偏差：旧 wire 透传 error 对象含 data）。 */
static cJSON *maths_py_call(maths_d_service_t *svc,
                                    const char *method, const cJSON *params,
                                    char *err, size_t err_sz)
{
    if (!maths_backend_available(&svc->py_backend)) {
        snprintf(err, err_sz,
                 "math backend unavailable (install maths-toolkit: "
                 "sh install.sh --with-maths)");
        return NULL;
    }
    if (!cJSON_IsObject(params)) {
        snprintf(err, err_sz, "params object required");
        return NULL;
    }

    char *params_json = cJSON_PrintUnformatted(params);
    if (!params_json) {
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }

    char resp[16384];
    int rc = maths_backend_call(&svc->py_backend, method, params_json, resp,
                                sizeof(resp));
    AIRY_FREE(params_json);
    if (rc != 0) {
        snprintf(err, err_sz, "symbolic backend call failed");
        return NULL;
    }

    cJSON *shell = cJSON_Parse(resp);
    if (!shell) {
        snprintf(err, err_sz, "malformed backend response");
        return NULL;
    }

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(shell, "error");
    cJSON *out = NULL;
    if (cJSON_IsObject(error)) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(error, "message");
        snprintf(err, err_sz, "%s",
                 cJSON_IsString(msg) ? msg->valuestring : "backend error");
    } else {
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(shell,
                                                               "result");
        if (cJSON_IsObject(result))
            out = cJSON_DetachItemViaPointer(shell, (cJSON *)result);
        else
            snprintf(err, err_sz, "malformed backend response");
    }
    cJSON_Delete(shell);

    if (out) {
        airy_mtx_lock(&svc->lock);
        svc->symbolic_count++;
        airy_mtx_unlock(&svc->lock);
    }
    return out;
}

maths_rpc_status_t maths_d_rpc_call(maths_d_service_t *svc,
                                    const char *method, const cJSON *params,
                                    cJSON **out_result, char *err,
                                    size_t err_sz)
{
    if (out_result)
        *out_result = NULL;
    if (!svc || !method || !out_result || !err || err_sz == 0)
        return MATHS_RPC_ERR_DOMAIN;

    cJSON *r = NULL;
    if (strcmp(method, "health_check") == 0)
        r = maths_health_result(svc);
    else if (strcmp(method, "get_stats") == 0)
        r = maths_usage_result(svc);
    else if (strcmp(method, "recognize") == 0) {
        int is_math = maths_d_recognize(maths_req_str(params, "text"));
        r = cJSON_CreateObject();
        if (r)
            cJSON_AddNumberToObject(r, "is_math", (double)is_math);
    } else if (strcmp(method, "eval") == 0) {
        r = maths_eval_result(svc, params, err, err_sz);
    } else if (strcmp(method, "stats") == 0) {
        r = maths_stats_call(svc, params, err, err_sz);
    } else if (strcmp(method, "plot") == 0) {
        r = maths_plot_result(svc, params, err, err_sz);
    } else if (maths_py_method(method)) {
        r = maths_py_call(svc, method, params, err, err_sz);
    } else {
        return MATHS_RPC_ERR_METHOD;
    }

    if (!r) {
        if (err[0] == '\0')
            snprintf(err, err_sz, "internal error");
        return MATHS_RPC_ERR_DOMAIN;
    }
    *out_result = r;
    return MATHS_RPC_OK;
}

/* ==================== 服务生命周期 ==================== */

int maths_d_service_init(maths_d_service_t *svc)
{
    if (!svc)
        return -1;
    AIRY_MEMSET(svc, 0, sizeof(*svc));
    airy_mtx_init(&svc->lock);
    atomic_store(&svc->running, 0);
    svc->start_time = (uint64_t)time(NULL);
    return 0;
}

void maths_d_service_destroy(maths_d_service_t *svc)
{
    if (!svc)
        return;
    airy_mtx_destroy(&svc->lock);
}
