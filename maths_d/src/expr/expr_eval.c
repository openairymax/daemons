/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file expr_eval.c
 * @brief 数学表达式求值引擎（递归下降解析器）。
 *
 * 从 maths_service.c 拆分（n11 大文件模块化，原文件 1020 行 → 本模块
 * 独立承载求值引擎）：纯 C 数值表达式求值，零外部解析库。
 *
 * 文法（右结合优先级）：
 *   expr   := term (('+'|'-') term)*
 *   term   := unary (('*'|'/'|'%') unary)*
 *   unary  := ('-'|'+') unary | power
 *   power  := primary ('^' unary)?          # 右结合
 *   postfix:= primary ('!')                 # 阶乘
 *   primary:= number | constant | variable | func '(' args ')' | '(' expr ')'
 *   variable: 单变量绑定（maths_d_eval_at，绘图采样求 y=f(x)）
 *
 * 函数表覆盖基础算术 + 初等函数（建议稿第一、二级）：sqrt/sin/cos/tan/
 * asin/acos/atan/exp/ln/log/log10/log2/abs/floor/ceil/round/sign/sinh/
 * cosh/tanh/cbrt/min/max/pow/atan2/mod/remainder/factorial，常量 pi/e。
 *
 * 安全边界：仅数值求值，无代码执行、无文件系统访问、无网络请求；
 * 表达式字符集白名单预检、长度 ≤4096、解析深度 ≤64、NaN/Inf 显式上报。
 */

#include "expr_eval.h"

#include "airy_memory.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MSVC /O2(/Oi) 会把 sqrt/sin 等 libm 符号 intrinsic 化，取地址不再是
 * 常量表达式 → 静态函数指针表 C2099（#122 实证 expr_eval.c(145,29)）。
 * pragma function 让其回落真实函数地址；须在文件作用域（函数内会触发
 * C2156），且仅 MSVC 需要（clang 对未知 pragma 告警，故守卫）。 */
#ifdef _MSC_VER
#pragma function(sqrt, sin, cos, tan, asin, acos, atan, exp, log, log10, log2, fabs, floor, ceil, round, sinh, cosh, tanh, cbrt)
#endif

/* ==================== 求值器状态 ==================== */

typedef struct {
    const char *s;        /* 剩余输入 */
    const char *err;      /* 错误消息（解析失败时） */
    int depth;            /* 递归深度 */
    const char *var;      /* 绑定变量名（NULL = 无变量绑定） */
    double var_val;       /* 绑定变量取值 */
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
static int maths_char_ok(int c)
{
    return isalnum(c) || strchr("+-*/%^!(),._ \t", c) != NULL;
}

static int maths_charset_ok(const char *expr)
{
    if (!expr || !expr[0])
        return 0;
    size_t n = 0;
    for (const char *p = expr; *p; p++) {
        if (!maths_char_ok((unsigned char)*p) || ++n > MATHS_MAX_EXPR_LEN)
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

static int eval_err(eval_ctx_t *ctx, const char *msg)
{
    ctx->err = msg;
    return -1;
}

static int eval_name_eq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && strncmp(s, lit, n) == 0;
}

/* 期待并消费右括号；msg 由调用点决定，保持各分支错误文案不变。 */
static int eval_take_rp(eval_ctx_t *ctx, const char *msg)
{
    eval_skip_ws(ctx);
    if (*ctx->s != ')')
        return eval_err(ctx, msg);
    ctx->s++;
    return 0;
}

/* 阶乘：仅接受 [0,170] 的整数（超出 double 精度边界）。 */
static int eval_fact(eval_ctx_t *ctx, double arg, double *out)
{
    if (arg < 0.0 || arg > 170.0)
        return eval_err(ctx, "factorial out of range");
    long long n = (long long)(arg + 1e-9);
    if (fabs(arg - (double)n) > 1e-6)
        return eval_err(ctx, "factorial requires integer");
    long double acc = 1.0L;
    for (long long i = 2; i <= n; i++)
        acc *= (long double)i;
    *out = (double)acc;
    return 0;
}

/* 单参调用：fn 非空直接求值；factorial 走整数阶乘；其余为不支持。 */
static int eval_call1(eval_ctx_t *ctx, const char *name, size_t nlen,
                      double (*fn)(double), double *out)
{
    double arg = 0.0;
    if (eval_parse_expr(ctx, &arg) != 0)
        return -1;
    if (eval_take_rp(ctx, "missing ')' in function call") != 0)
        return -1;
    if (fn)
        *out = fn(arg);
    else if (eval_name_eq(name, nlen, "factorial"))
        return eval_fact(ctx, arg, out);
    else
        return eval_err(ctx, "unsupported function");
    return 0;
}

/* log(x) 自然对数 / log(x,b) 通用对数：底数非法时退化为自然对数。 */
static int eval_call_log(eval_ctx_t *ctx, double *out)
{
    double a = 0.0, b = 0.0;
    if (eval_parse_expr(ctx, &a) != 0)
        return -1;
    eval_skip_ws(ctx);
    if (*ctx->s == ',') {
        ctx->s++;
        if (eval_parse_expr(ctx, &b) != 0)
            return -1;
    }
    if (eval_take_rp(ctx, "missing ')' in log()") != 0)
        return -1;
    *out = (b > 0.0 && b != 1.0) ? log(a) / log(b) : log(a);
    return 0;
}

/* 双参调用：先按文法消费两个实参与右括号，再校验 arity。 */
static int eval_call2(eval_ctx_t *ctx, const char *name, size_t nlen,
                      int arity, double *out)
{
    double a = 0.0, b = 0.0;
    if (eval_parse_expr(ctx, &a) != 0)
        return -1;
    eval_skip_ws(ctx);
    if (*ctx->s != ',')
        return eval_err(ctx, "missing ',' in function call");
    ctx->s++;
    if (eval_parse_expr(ctx, &b) != 0)
        return -1;
    if (eval_take_rp(ctx, "missing ')' in function call") != 0)
        return -1;
    if (arity != 2)
        return eval_err(ctx, "unsupported function arity");
    if (eval_name_eq(name, nlen, "min"))
        *out = a < b ? a : b;
    else if (eval_name_eq(name, nlen, "max"))
        *out = a > b ? a : b;
    else if (eval_name_eq(name, nlen, "pow"))
        *out = pow(a, b);
    else if (eval_name_eq(name, nlen, "atan2"))
        *out = atan2(a, b);
    else if (eval_name_eq(name, nlen, "mod"))
        *out = fmod(a, b);
    else if (eval_name_eq(name, nlen, "remainder"))
        *out = remainder(a, b);
    else
        *out = 0.0;
    return 0;
}

/* 函数调用分派：arity 1 单参、-2 为 log 族、其余走双参路径。 */
static int eval_parse_call(eval_ctx_t *ctx, const char *name, size_t nlen,
                           double *out)
{
    double (*fn)(double) = NULL;
    int arity = 0;
    eval_skip_ws(ctx);
    if (*ctx->s != '(')
        return eval_err(ctx, "unknown identifier");
    if (eval_lookup_func(name, nlen, &fn, &arity) != 0)
        return eval_err(ctx, "unknown function");
    ctx->s++;
    if (arity == 1)
        return eval_call1(ctx, name, nlen, fn, out);
    if (arity == -2)
        return eval_call_log(ctx, out);
    return eval_call2(ctx, name, nlen, arity, out);
}

/* 标识符：常量 pi/e、绑定变量，否则按函数调用处理。 */
static int eval_parse_ident(eval_ctx_t *ctx, double *out)
{
    const char *start = ctx->s;
    while (maths_is_ident_char(*ctx->s))
        ctx->s++;
    size_t nlen = (size_t)(ctx->s - start);
    if (eval_name_eq(start, nlen, "pi")) {
        *out = 3.14159265358979323846;
        return 0;
    }
    if (nlen == 1 && *start == 'e') {
        *out = 2.71828182845904523536;
        return 0;
    }
    if (ctx->var && eval_name_eq(start, nlen, ctx->var)) {
        *out = ctx->var_val;
        return 0;
    }
    return eval_parse_call(ctx, start, nlen, out);
}

static int eval_parse_paren(eval_ctx_t *ctx, double *out)
{
    ctx->s++;
    if (eval_parse_expr(ctx, out) != 0)
        return -1;
    return eval_take_rp(ctx, "missing ')'");
}

static int eval_parse_primary(eval_ctx_t *ctx, double *out)
{
    eval_skip_ws(ctx);
    if (++ctx->depth > MATHS_MAX_DEPTH)
        return eval_err(ctx, "expression too deep");

    int rc;
    if (*ctx->s == '(')
        rc = eval_parse_paren(ctx, out);
    else if (maths_is_ident_start(*ctx->s))
        rc = eval_parse_ident(ctx, out);
    else if (maths_is_digit_char(*ctx->s))
        rc = eval_parse_number(ctx, out) != 0 ? eval_err(ctx, "invalid number") : 0;
    else
        return eval_err(ctx, "unexpected token");
    if (rc == 0)
        ctx->depth--;
    return rc;
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
        ctx->s++;
        return eval_fact(ctx, *out, out);
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

int maths_d_eval_at(const char *expr, const char *var_name, double var_value,
                    double *out_result, char *err_msg, size_t err_msg_size)
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
    ctx.var = var_name;
    ctx.var_val = var_value;

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

int maths_d_eval(const char *expr, double *out_result, char *err_msg,
                 size_t err_msg_size)
{
    return maths_d_eval_at(expr, NULL, 0.0, out_result, err_msg, err_msg_size);
}
