/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file maths_service.c
 * @brief 数学外挂计算服务核心实现。
 *
 * 递归下降求值器（纯 C，无外部解析库）：
 *   expr   := term (('+'|'-') term)*
 *   term   := unary (('*'|'/'|'%') unary)*
 *   unary  := ('-'|'+') unary | power
 *   power  := primary ('^' unary)?          # 右结合
 *   postfix:= primary ('!' | '!')
 *   primary:= number | constant | func '(' args ')' | '(' expr ')'
 *
 * 函数表覆盖基础算术 + 初等函数（建议稿第一、二级），统计覆盖
 * 描述性统计（第五级）。线性代数/符号计算不在本服务范围，由上层
 * 路由到外部可插拔服务。
 */

#include "maths_service.h"
#include "airy_memory.h"
#include "logging.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || defined(__WIN32__)
#include <winsock2.h>
#endif

/* ==================== 求值器状态 ==================== */

typedef struct {
    const char *s;        /* 剩余输入 */
    const char *err;      /* 错误消息（解析失败时） */
    int depth;            /* 递归深度 */
} eval_ctx_t;

/* 标识符首字符只能是字母/下划线；后续可含数字（log10/log2） */
static int maths_is_ident_start(int c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static int maths_is_ident_char(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int maths_is_digit_char(int c)
{
    return isdigit((unsigned char)c) || c == '.';
}

static void eval_skip_ws(eval_ctx_t *ctx)
{
    while (*ctx->s == ' ' || *ctx->s == '\t')
        ctx->s++;
}

/* 合法表达式字符预检：把明显非数学的文本挡在解析之前（防注入与空转）。
 * 允许：数字、运算符、括号、逗号、点、字母（函数/常量）、空白。 */
static int maths_charset_ok(const char *expr)
{
    if (!expr || !expr[0])
        return 0;
    size_t n = 0;
    for (const char *p = expr; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '+' || c == '-' || c == '*' || c == '/' ||
              c == '%' || c == '^' || c == '!' || c == '(' || c == ')' ||
              c == ',' || c == '.' || c == '_' || c == ' ' || c == '\t' ||
              c == 'e' || c == 'E'))
            return 0;
        if (++n > MATHS_MAX_EXPR_LEN)
            return 0;
    }
    return 1;
}

/* 数字字面量：123 / 1.5 / 2e3 / 1.2e-4 */
static int eval_parse_number(eval_ctx_t *ctx, double *out)
{
    const char *start = ctx->s;
    int has_digit = 0;

    while (isdigit((unsigned char)*ctx->s)) {
        ctx->s++;
        has_digit = 1;
    }
    if (*ctx->s == '.') {
        ctx->s++;
        while (isdigit((unsigned char)*ctx->s)) {
            ctx->s++;
            has_digit = 1;
        }
    }
    if (!has_digit)
        return -1;

    if (*ctx->s == 'e' || *ctx->s == 'E') {
        const char *save = ctx->s;
        ctx->s++;
        if (*ctx->s == '+' || *ctx->s == '-')
            ctx->s++;
        if (!isdigit((unsigned char)*ctx->s)) {
            ctx->s = save; /* 不是科学计数，回退 */
        } else {
            while (isdigit((unsigned char)*ctx->s))
                ctx->s++;
        }
    }

    char tmp[64];
    size_t len = (size_t)(ctx->s - start);
    if (len >= sizeof(tmp))
        return -1;
    AIRY_MEMCPY(tmp, start, len);
    tmp[len] = '\0';
    *out = strtod(tmp, NULL);
    return 0;
}

/* 数学常量与函数查找（名称 → 单参数/多参数函数）。返回 -2 表示需走函数
 * 调用路径（即已知但参数非单值），-1 未知名。 */
static double maths_sign(double x)
{
    return x < 0.0 ? -1.0 : (x > 0.0 ? 1.0 : 0.0);
}

static int eval_lookup_func(const char *name, size_t nlen, double (**out_fn1)(double),
                            int *out_arity)
{
    struct fn1 {
        const char *name;
        double (*fn)(double);
    };
    static const struct fn1 tbl1[] = {
        { "sqrt", sqrt },       { "sin", sin },     { "cos", cos },
        { "tan", tan },         { "asin", asin },   { "acos", acos },
        { "atan", atan },       { "exp", exp },     { "ln", log },
        { "log10", log10 },     { "log2", log2 },   { "abs", fabs },
        { "floor", floor },     { "ceil", ceil },   { "round", round },
        { "sign", maths_sign }, { "sinh", sinh },   { "cosh", cosh },
        { "tanh", tanh },       { "cbrt", cbrt },
    };

    for (size_t i = 0; i < sizeof(tbl1) / sizeof(tbl1[0]); i++) {
        if (strlen(tbl1[i].name) == nlen && strncmp(tbl1[i].name, name, nlen) == 0) {
            *out_fn1 = tbl1[i].fn;
            *out_arity = 1;
            return 0;
        }
    }

    /* 多参数/特殊函数：走 arity 2 路径（min/max/pow/log/atan2/remainder）
     * 或阶乘等特殊处理 */
    struct fn2 {
        const char *name;
        int arity;
    };
    static const struct fn2 tbl2[] = {
        { "min", 2 },   { "max", 2 },   { "pow", 2 },    { "log", -2 },
        { "atan2", 2 }, { "mod", 2 },   { "remainder", 2 }, { "factorial", 1 },
    };
    for (size_t i = 0; i < sizeof(tbl2) / sizeof(tbl2[0]); i++) {
        if (strlen(tbl2[i].name) == nlen && strncmp(tbl2[i].name, name, nlen) == 0) {
            *out_fn1 = NULL;
            *out_arity = tbl2[i].arity;
            return 0;
        }
    }

    (void)out_fn1;
    return -1;
}

static int eval_parse_expr(eval_ctx_t *ctx, double *out);
static void maths_build_error_response(char *resp, size_t resp_sz, int id,
                                       const char *msg);

static int eval_parse_primary(eval_ctx_t *ctx, double *out)
{
    eval_skip_ws(ctx);

    if (++ctx->depth > MATHS_MAX_DEPTH) {
        ctx->err = "expression too deep";
        return -1;
    }

    if (*ctx->s == '(') {
        ctx->s++;
        if (eval_parse_expr(ctx, out) != 0)
            return -1;
        eval_skip_ws(ctx);
        if (*ctx->s != ')') {
            ctx->err = "missing ')'";
            return -1;
        }
        ctx->s++;
        ctx->depth--;
        return 0;
    }

    /* 常量 pi/e */
    if (maths_is_ident_start(*ctx->s)) {
        const char *start = ctx->s;
        while (maths_is_ident_char(*ctx->s))
            ctx->s++;
        size_t nlen = (size_t)(ctx->s - start);

        if (nlen == 2 && strncmp(start, "pi", 2) == 0) {
            *out = 3.14159265358979323846;
            ctx->depth--;
            return 0;
        }
        if (nlen == 1 && *start == 'e') {
            *out = 2.71828182845904523536;
            ctx->depth--;
            return 0;
        }

        /* 函数调用 */
        double (*fn1)(double) = NULL;
        int arity = 0;
        eval_skip_ws(ctx);
        if (*ctx->s != '(') {
            ctx->err = "unknown identifier";
            return -1;
        }
        if (eval_lookup_func(start, nlen, &fn1, &arity) != 0) {
            ctx->err = "unknown function";
            return -1;
        }
        ctx->s++; /* 跳过 '(' */

        if (arity == 1) {
            double arg = 0.0;
            if (eval_parse_expr(ctx, &arg) != 0)
                return -1;
            eval_skip_ws(ctx);
            if (*ctx->s != ')') {
                ctx->err = "missing ')' in function call";
                return -1;
            }
            ctx->s++;
            if (fn1) {
                *out = fn1(arg);
            } else {
                /* factorial：整数阶乘（tbl2 特判） */
                if (nlen == 9 && strncmp(start, "factorial", 9) == 0) {
                    if (arg < 0.0 || arg > 170.0) {
                        ctx->err = "factorial out of range";
                        return -1;
                    }
                    long long n = (long long)(arg + 1e-9);
                    if (fabs(arg - (double)n) > 1e-6) {
                        ctx->err = "factorial requires integer";
                        return -1;
                    }
                    long double acc = 1.0L;
                    for (long long i = 2; i <= n; i++)
                        acc *= (long double)i;
                    *out = (double)acc;
                } else {
                    ctx->err = "unsupported function";
                    return -1;
                }
            }
            ctx->depth--;
            return 0;
        }

        if (arity == -2) {
            /* log(x) 自然对数 / log(x,b) 通用对数 */
            double a = 0.0, b = 0.0;
            if (eval_parse_expr(ctx, &a) != 0)
                return -1;
            eval_skip_ws(ctx);
            if (*ctx->s == ',') {
                ctx->s++;
                if (eval_parse_expr(ctx, &b) != 0)
                    return -1;
            } else {
                b = 0.0;
            }
            eval_skip_ws(ctx);
            if (*ctx->s != ')') {
                ctx->err = "missing ')' in log()";
                return -1;
            }
            ctx->s++;
            if (b > 0.0 && b != 1.0)
                *out = log(a) / log(b);
            else
                *out = log(a);
            ctx->depth--;
            return 0;
        }

        {
            /* 双参数函数 */
            double args[2] = { 0.0, 0.0 };
            if (eval_parse_expr(ctx, &args[0]) != 0)
                return -1;
            eval_skip_ws(ctx);
            if (*ctx->s != ',') {
                ctx->err = "missing ',' in function call";
                return -1;
            }
            ctx->s++;
            if (eval_parse_expr(ctx, &args[1]) != 0)
                return -1;
            eval_skip_ws(ctx);
            if (*ctx->s != ')') {
                ctx->err = "missing ')' in function call";
                return -1;
            }
            ctx->s++;

            if (arity == 2) {
                if (nlen == 3 && strncmp(start, "min", 3) == 0)
                    *out = args[0] < args[1] ? args[0] : args[1];
                else if (nlen == 3 && strncmp(start, "max", 3) == 0)
                    *out = args[0] > args[1] ? args[0] : args[1];
                else if (nlen == 3 && strncmp(start, "pow", 3) == 0)
                    *out = pow(args[0], args[1]);
                else if (nlen == 5 && strncmp(start, "atan2", 5) == 0)
                    *out = atan2(args[0], args[1]);
                else if (nlen == 3 && strncmp(start, "mod", 3) == 0)
                    *out = fmod(args[0], args[1]);
                else if (nlen == 9 && strncmp(start, "remainder", 9) == 0)
                    *out = remainder(args[0], args[1]);
                else
                    *out = 0.0;
            } else {
                ctx->err = "unsupported function arity";
                return -1;
            }
            ctx->depth--;
            return 0;
        }
    }

    if (maths_is_digit_char(*ctx->s) ||
        (*ctx->s == '.' && isdigit((unsigned char)ctx->s[1]))) {
        if (eval_parse_number(ctx, out) != 0) {
            ctx->err = "invalid number";
            return -1;
        }
        ctx->depth--;
        return 0;
    }

    ctx->err = "unexpected token";
    return -1;
}

/* 幂（右结合）+ 阶乘后缀 */
static int eval_parse_power(eval_ctx_t *ctx, double *out)
{
    if (eval_parse_primary(ctx, out) != 0)
        return -1;

    eval_skip_ws(ctx);
    if (*ctx->s == '^') {
        ctx->s++;
        double rhs = 0.0;
        if (eval_parse_power(ctx, &rhs) != 0)
            return -1;
        *out = pow(*out, rhs);
    } else if (*ctx->s == '!') {
        /* 阶乘：仅对非负整数有效（float 输入向下取整并告警） */
        ctx->s++;
        if (*out < 0.0 || *out > 170.0) {
            ctx->err = "factorial out of range";
            return -1;
        }
        long long n = (long long)(*out + 1e-9);
        if (fabs(*out - (double)n) > 1e-6) {
            ctx->err = "factorial requires integer";
            return -1;
        }
        long double acc = 1.0L;
        for (long long i = 2; i <= n; i++)
            acc *= (long double)i;
        *out = (double)acc;
    }
    return 0;
}

/* 一元正负号 */
static int eval_parse_unary(eval_ctx_t *ctx, double *out)
{
    eval_skip_ws(ctx);
    if (*ctx->s == '-') {
        ctx->s++;
        if (eval_parse_unary(ctx, out) != 0)
            return -1;
        *out = -*out;
        return 0;
    }
    if (*ctx->s == '+') {
        ctx->s++;
        if (eval_parse_unary(ctx, out) != 0)
            return -1;
        return 0;
    }
    return eval_parse_power(ctx, out);
}

static int eval_parse_term(eval_ctx_t *ctx, double *out)
{
    if (eval_parse_unary(ctx, out) != 0)
        return -1;

    for (;;) {
        eval_skip_ws(ctx);
        if (*ctx->s == '*') {
            ctx->s++;
            double rhs = 0.0;
            if (eval_parse_unary(ctx, &rhs) != 0)
                return -1;
            *out *= rhs;
        } else if (*ctx->s == '/') {
            ctx->s++;
            double rhs = 0.0;
            if (eval_parse_unary(ctx, &rhs) != 0)
                return -1;
            if (rhs == 0.0) {
                ctx->err = "division by zero";
                return -1;
            }
            *out /= rhs;
        } else if (*ctx->s == '%') {
            ctx->s++;
            double rhs = 0.0;
            if (eval_parse_unary(ctx, &rhs) != 0)
                return -1;
            if (rhs == 0.0) {
                ctx->err = "modulo by zero";
                return -1;
            }
            *out = fmod(*out, rhs);
        } else {
            break;
        }
    }
    return 0;
}

static int eval_parse_expr(eval_ctx_t *ctx, double *out)
{
    if (eval_parse_term(ctx, out) != 0)
        return -1;

    for (;;) {
        eval_skip_ws(ctx);
        if (*ctx->s == '+') {
            ctx->s++;
            double rhs = 0.0;
            if (eval_parse_term(ctx, &rhs) != 0)
                return -1;
            *out += rhs;
        } else if (*ctx->s == '-') {
            ctx->s++;
            double rhs = 0.0;
            if (eval_parse_term(ctx, &rhs) != 0)
                return -1;
            *out -= rhs;
        } else {
            break;
        }
    }
    return 0;
}

int maths_d_eval(const char *expr, double *out_result, char *err_msg,
                 size_t err_msg_size)
{
    if (!expr || !out_result || !err_msg || err_msg_size == 0)
        return -1;

    if (!maths_charset_ok(expr)) {
        snprintf(err_msg, err_msg_size, "expression contains invalid characters");
        return -1;
    }

    eval_ctx_t ctx;
    ctx.s = expr;
    ctx.err = NULL;
    ctx.depth = 0;

    double result = 0.0;
    if (eval_parse_expr(&ctx, &result) != 0) {
        snprintf(err_msg, err_msg_size, "%s", ctx.err ? ctx.err : "parse error");
        return -1;
    }

    eval_skip_ws(&ctx);
    if (*ctx.s != '\0') {
        snprintf(err_msg, err_msg_size, "unexpected trailing input at '%s'", ctx.s);
        return -1;
    }

    if (isnan(result) || isinf(result)) {
        snprintf(err_msg, err_msg_size, "result is not finite (NaN/Inf)");
        return -1;
    }

    *out_result = result;
    return 0;
}

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

/* ==================== JSON-RPC 分发 ==================== */

/* 简易 JSON 字段提取（无 cJSON 依赖时用）：{"key":value}。 */
static int json_get_string(const char *json, const char *key, char *out,
                           size_t out_size)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') {
                out[n++] = '\n';
                p++;
                continue;
            }
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return (n > 0) ? 0 : -1;
}

static int json_get_array(const char *json, const char *key, double *out,
                          size_t max, size_t *out_count)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '[')
        return -1;
    p++;

    size_t count = 0;
    while (*p && *p != ']' && count < max) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '[')
            p++;
        if (*p == ']' || *p == '\0')
            break;
        out[count++] = strtod(p, (char **)&p);
    }
    if (out_count)
        *out_count = count;
    return 0;
}

/* 提取 JSON 对象字段：{"key":{...}}，把 {..}（含花括号）拷到 out。
 * 支持对象内嵌套花括号（按深度匹配）。返回 0 成功。 */
static int json_get_object(const char *json, const char *key, char *out,
                           size_t out_size)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '{')
        return -1;

    int depth = 0;
    size_t n = 0;
    int in_str = 0;
    while (*p && n + 1 < out_size) {
        char c = *p;
        out[n++] = c;
        if (c == '"' && (n < 2 || out[n - 2] != '\\'))
            in_str = !in_str;
        if (!in_str) {
            if (c == '{')
                depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) {
                    out[n] = '\0';
                    return 0;
                }
            }
        }
        p++;
    }
    out[n] = '\0';
    return -1;
}

/* 后端响应转发：后端返回 {"id":1,"result":{...}} 或 {"id":1,"error":{...}}，
 * 这里按请求 id 重新构造标准 JSON-RPC 响应。 */
static void maths_forward_symbolic_response(const char *backend_resp, int id,
                                            char *response,
                                            size_t response_size)
{
    char obj[8192];
    if (json_get_object(backend_resp, "error", obj, sizeof(obj)) == 0) {
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":%s}", id, obj);
        return;
    }
    if (json_get_object(backend_resp, "result", obj, sizeof(obj)) == 0) {
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, obj);
        return;
    }
    maths_build_error_response(response, response_size, id,
                               "malformed backend response");
}

static void maths_build_error_response(char *resp, size_t resp_sz, int id,
                                       const char *msg)
{
    snprintf(resp, resp_sz,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32000,"
             "\"message\":\"%s\"}}",
             id, msg);
}

int maths_d_dispatch_jsonrpc(maths_d_service_t *svc, const char *request,
                             char *response, size_t response_size)
{
    if (!svc || !request || !response || response_size == 0)
        return MATHS_METHOD_NOT_RPC;

    char method[64] = "";
    if (json_get_string(request, "method", method, sizeof(method)) != 0)
        return MATHS_METHOD_NOT_RPC;

    int id = 0;
    char id_buf[32] = "";
    if (json_get_string(request, "id", id_buf, sizeof(id_buf)) == 0)
        id = atoi(id_buf);

    if (strcmp(method, "shutdown") == 0) {
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"status\":\"shutting_down\"}}",
                 id);
        return MATHS_METHOD_SHUTDOWN;
    }

    if (strcmp(method, "health_check") == 0) {
        airy_mtx_lock(&svc->lock);
        int healthy = atomic_load(&svc->running) ? 1 : 0;
        uint64_t evals = svc->eval_count;
        uint64_t errors = svc->error_count;
        uint64_t last_ms = svc->last_eval_ms;
        airy_mtx_unlock(&svc->lock);
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"status\":\"%s\","
                 "\"service\":\"maths_d\",\"eval_count\":%llu,\"error_count\":%llu,"
                 "\"last_eval_ms\":%llu}}",
                 id, healthy ? "healthy" : "degraded", (unsigned long long)evals,
                 (unsigned long long)errors, (unsigned long long)last_ms);
        return MATHS_METHOD_HANDLED;
    }

    if (strcmp(method, "get_stats") == 0) {
        airy_mtx_lock(&svc->lock);
        uint64_t evals = svc->eval_count;
        uint64_t stats = svc->stats_count;
        uint64_t symbolic = svc->symbolic_count;
        uint64_t errors = svc->error_count;
        uint64_t last_ms = svc->last_eval_ms;
        uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
        int backend_up = maths_backend_available(&svc->py_backend);
        airy_mtx_unlock(&svc->lock);
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"service\":\"maths_d\","
                 "\"eval_count\":%llu,\"stats_count\":%llu,\"symbolic_count\":%llu,"
                 "\"python_backend\":\"%s\",\"error_count\":%llu,"
                 "\"last_eval_ms\":%llu,\"uptime_sec\":%llu}}",
                 id, (unsigned long long)evals, (unsigned long long)stats,
                 (unsigned long long)symbolic, backend_up ? "up" : "degraded",
                 (unsigned long long)errors, (unsigned long long)last_ms,
                 (unsigned long long)uptime);
        return MATHS_METHOD_HANDLED;
    }

    if (strcmp(method, "recognize") == 0) {
        char text[MATHS_MAX_EXPR_LEN] = "";
        json_get_string(request, "text", text, sizeof(text));
        int is_math = maths_d_recognize(text);
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"is_math\":%d}}", id,
                 is_math);
        return MATHS_METHOD_HANDLED;
    }

    if (strcmp(method, "eval") == 0) {
        char expr[MATHS_MAX_EXPR_LEN] = "";
        if (json_get_string(request, "expr", expr, sizeof(expr)) != 0) {
            maths_build_error_response(response, response_size, id,
                                       "missing expr");
            return MATHS_METHOD_HANDLED;
        }
        double result = 0.0;
        char err[128] = "";
        uint64_t t0 = (uint64_t)time(NULL);
        int rc = maths_d_eval(expr, &result, err, sizeof(err));
        uint64_t elapsed_ms = ((uint64_t)time(NULL) - t0) * 1000;

        airy_mtx_lock(&svc->lock);
        svc->last_eval_ms = elapsed_ms;
        if (rc == 0)
            svc->eval_count++;
        else
            svc->error_count++;
        airy_mtx_unlock(&svc->lock);

        if (rc != 0) {
            maths_build_error_response(response, response_size, id, err);
            return MATHS_METHOD_HANDLED;
        }
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"expr\":\"%s\","
                 "\"result\":%.12g,\"elapsed_ms\":%llu}}",
                 id, expr, result, (unsigned long long)elapsed_ms);
        return MATHS_METHOD_HANDLED;
    }

    if (strcmp(method, "stats") == 0) {
        char op[32] = "";
        double values[MATHS_MAX_VALUES];
        size_t count = 0;
        if (json_get_string(request, "op", op, sizeof(op)) != 0 ||
            json_get_array(request, "values", values, MATHS_MAX_VALUES, &count) != 0 ||
            count == 0) {
            maths_build_error_response(response, response_size, id,
                                       "invalid stats params (op + values[])");
            return MATHS_METHOD_HANDLED;
        }
        double result = 0.0;
        char err[128] = "";
        int rc = maths_d_stats(op, values, count, &result, err, sizeof(err));

        airy_mtx_lock(&svc->lock);
        if (rc == 0)
            svc->stats_count++;
        else
            svc->error_count++;
        airy_mtx_unlock(&svc->lock);

        if (rc != 0) {
            maths_build_error_response(response, response_size, id, err);
            return MATHS_METHOD_HANDLED;
        }
        snprintf(response, response_size,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"op\":\"%s\","
                 "\"count\":%zu,\"result\":%.12g}}",
                 id, op, count, result);
        return MATHS_METHOD_HANDLED;
    }

    /* ---- 符号计算方法（solve/differentiate/integrate/limit/simplify/
     *       factor/expand/matrix/units）委托 Python 后端（maths-toolkit）---- */
    if (strcmp(method, "solve") == 0 || strcmp(method, "differentiate") == 0 ||
        strcmp(method, "integrate") == 0 || strcmp(method, "limit") == 0 ||
        strcmp(method, "simplify") == 0 || strcmp(method, "factor") == 0 ||
        strcmp(method, "expand") == 0 || strcmp(method, "matrix") == 0 ||
        strcmp(method, "units") == 0) {
        if (!maths_backend_available(&svc->py_backend)) {
            maths_build_error_response(
                response, response_size, id,
                "symbolic backend unavailable (install maths-toolkit: "
                "sh install.sh --with-maths)");
            return MATHS_METHOD_HANDLED;
        }

        char params[4096] = "{}";
        if (json_get_object(request, "params", params, sizeof(params)) != 0) {
            /* 无 params 或解析失败时后端按缺省处理 */
        }

        char resp[16384];
        if (maths_backend_call(&svc->py_backend, method, params, resp,
                               sizeof(resp)) != 0) {
            maths_build_error_response(response, response_size, id,
                                       "symbolic backend call failed");
            return MATHS_METHOD_HANDLED;
        }

        airy_mtx_lock(&svc->lock);
        svc->symbolic_count++;
        airy_mtx_unlock(&svc->lock);

        maths_forward_symbolic_response(resp, id, response, response_size);
        return MATHS_METHOD_HANDLED;
    }

    maths_build_error_response(response, response_size, id, "method not found");
    return MATHS_METHOD_HANDLED;
}

/* ==================== 服务生命周期 ==================== */

int maths_d_service_init(maths_d_service_t *svc)
{
    if (!svc)
        return -1;
    AIRY_MEMSET(svc, 0, sizeof(*svc));
    svc->server_fd = AIRY_INVALID_SOCKET;
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
    AIRY_FREE(svc->socket_path);
    svc->socket_path = NULL;
}
