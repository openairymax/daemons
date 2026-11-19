// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_loop.c
 * @brief Agent run 工具循环（ReAct，M1-1a 引擎下沉）。
 *
 * 自 gateway_biz_llm.c 迁移：LLM complete_stream -> 增量正文即时上屏 +
 * tool_calls -> tool_d 执行 -> 回填上下文 -> 继续。工具 schema 单一权威经
 * commons 契约层（airy_tool_schema.h）获取；llm_d/tool_d 经
 * daemon_rpc_client 直连；RS 分帧解析由 agent_run_stream.c 承担。
 */

#include "agent_run_internal.h"

#include "agent_run_alt.h"
#include "agent_turn_ckpt.h"
#include "airy_memory.h"
#include "airy_run_stream.h"
#include "airy_tool_schema.h"
#include "daemon_rpc_client.h"
#include "err_class.h"
#include "platform.h"
#include "safe_utf8.h"
#include "svc_logger.h"
#include "text_utils.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_RUN_SOCK_BUF AIRY_PATH_MAX

/* 增量上屏的栈缓冲上限：常见逐字/逐片增量远小于此值，超限走堆。 */
#define RUN_DELTA_STACK 1024

/* 轮次预算解析：请求值优先；未给则读策略面环境变量；再缺省 8；
 * 最后夹取到机制硬上限 32。策略只能收紧或放宽到上限，不能突破。 */
int rounds_resolve(int requested)
{
    int n = requested;
    if (n <= 0) {
        const char *env = getenv("AIRY_AGENT_TOOL_ROUNDS");
        if (env && *env)
            n = atoi(env);
    }
    if (n <= 0)
        n = AGENT_RUN_ROUNDS_DEFAULT;
    if (n > AGENT_RUN_ROUNDS_MAX)
        n = AGENT_RUN_ROUNDS_MAX;
    return n;
}

/* D-10：按原始退出码分类失败。只描述语义，不改码；retryable 表示原样重发
 * 是否安全，next_action 给出客户端下一步（重试/续跑/人工介入）。
 *
 * 重试策略（客户端一律读载荷字段，不得按文案自行推断）：
 *   成因             origin    reason            retryable  next_action
 *   预算耗尽         agent     budget            1          resume
 *   工具熔断         tool      tool              0          inspect
 *   审批拒绝         tool      denied            0          inspect
 *   上游不可达       provider  provider          1          retry
 *   上游可达但报错   provider  provider_error    0          inspect
 * retryable 只表示「原样重发是否安全」，不等于能推进：budget 必须用令牌
 * 续跑，重发同一 prompt 只会再耗一轮预算；denied 需人工放行后重提。 */
agent_fail_desc_t agent_fail_desc(int rc, int denied)
{
    agent_fail_desc_t d = {AIRY_RS_ORIGIN_AGENT, AIRY_RS_REASON_AGENT, 0,
                           AIRY_RS_NEXT_INSPECT};
    if (denied && rc == AGENT_RUN_RC_TOOL_FUSE) {
        /* 熔断由拒绝引起时归因为审批，而非「工具坏了」。 */
        d.origin = AIRY_RS_ORIGIN_TOOL;
        d.reason = AIRY_RS_REASON_DENIED;
        return d;
    }
    switch (rc) {
    case AIRY_EBUDGET:
        d.reason = AIRY_RS_REASON_BUDGET;
        d.retryable = 1;
        d.next_action = AIRY_RS_NEXT_RESUME;
        break;
    case AGENT_RUN_RC_TOOL_FUSE:
        d.origin = AIRY_RS_ORIGIN_TOOL;
        d.reason = AIRY_RS_REASON_TOOL;
        break;
    case AGENT_RUN_RC_EMPTY:
        d.reason = AIRY_RS_REASON_EMPTY;
        d.retryable = 1;
        d.next_action = AIRY_RS_NEXT_RETRY;
        break;
    case AGENT_RUN_RC_UNMET:
        /* 判据未过是确定性事实：证据已随 verification 下发，重发同一请求
         * 不会改变结果，必须按逐项 expect/actual 修复产物。 */
        d.origin = AIRY_RS_ORIGIN_VERIFIER;
        d.reason = AIRY_RS_REASON_UNMET;
        d.next_action = AIRY_RS_NEXT_ADJUST;
        break;
    case AGENT_RUN_RC_UNVERIFIED:
        d.origin = AIRY_RS_ORIGIN_VERIFIER;
        d.reason = AIRY_RS_REASON_UNVERIFIED;
        d.retryable = 1;
        break;
    case -1:
        d.origin = AIRY_RS_ORIGIN_PROVIDER;
        d.reason = AIRY_RS_REASON_PROVIDER;
        d.retryable = 1;
        d.next_action = AIRY_RS_NEXT_RETRY;
        break;
    case AGENT_RUN_RC_LLM_FAIL:
        /* 上游失败：配置错（鉴权/模型名）重试无用，但传输与 5xx 可重试。
         * 明细在 fail_msg，客户端据此选择「修复配置」或「重试」。 */
        d.origin = AIRY_RS_ORIGIN_PROVIDER;
        d.reason = AIRY_RS_REASON_PROVIDER;
        d.retryable = 1;
        d.next_action = AIRY_RS_NEXT_RETRY;
        break;
    default:
        break;
    }
    return d;
}

/* 构建 llm_d complete_stream 请求（透传本轮工具目录与常规生成参数）。
 * 不在此处写 max_tokens：输出上限的权威在配置面（model.yaml max_output），
 * 由 llm_d 的生成参数解析统一裁决；调用方写死一个数值会让该配置永久失效。
 * tools_src 是 run_catalog_load 产出的本轮目录快照（OpenAI tools 条目）。
 * has_snapshot 区分「快照为空」与「从未取到快照」：前者如实下发无工具，
 * 后者才回落静态 SSoT bootstrap（事件标 catalog_source=static）。 */
static char *run_build_llm_params(const char *model, const cJSON *messages, const cJSON *tools_src,
                                  int has_snapshot)
{
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return NULL;
    cJSON_AddStringToObject(params, "model", model);
    cJSON_AddItemToObject(params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = has_snapshot ? cJSON_Duplicate(tools_src, 1)
                                : cJSON_Parse(AIRY_TOOLS_JSON_SOURCE);
    /* 空目录不带 tools 键：部分 provider 拒收空数组，且「没有可用工具」
     * 与「没给工具定义」在模型侧等价。 */
    if (tools && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(params, "tools", tools);
    else
        cJSON_Delete(tools);
    cJSON_AddNumberToObject(params, "temperature", 0.7);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    return params_str;
}

/* token_delta 推送上下文（流式期间由解帧器逐增量回调）。 */
typedef struct {
    const agent_run_event_sink_t *sink;
    uint64_t *seq;
    const char *run_id;
    const char *sess;
} run_delta_ctx_t;

/* 真实增量即时上屏：载荷按解帧器给出的长度全量承载（不截断、不分片丢内容）；
 * 空增量不产生事件，保证帧数与解帧器回调次数一一对应。
 * dlen 是权威长度：不依赖缓冲区在 dlen 处已置 NUL（长度超过栈缓冲时走堆）。 */
static void run_emit_delta(const char *delta, size_t dlen, void *ud)
{
    run_delta_ctx_t *c = (run_delta_ctx_t *)ud;
    if (!c || !c->sink || !c->sink->emit || !delta || dlen == 0)
        return;

    char stackbuf[RUN_DELTA_STACK];
    char *buf = dlen < sizeof(stackbuf) ? stackbuf : (char *)AIRY_MALLOC(dlen + 1);
    if (!buf)
        return;
    AIRY_MEMCPY(buf, delta, dlen);
    buf[dlen] = '\0';

    cJSON *td = cJSON_CreateObject();
    if (td) {
        cJSON_AddStringToObject(td, AIRY_RS_K_DELTA, buf);
        agent_run_emit_event(c->sink, c->seq, c->run_id, c->sess, AIRY_RS_TYPE_TOKEN_DELTA, td);
    }
    if (buf != stackbuf)
        AIRY_FREE(buf);
}

/* 释放一次流消费的产物（已接管字段调用方先行摘除）。 */
static void run_stream_result_free(agent_stream_result_t *sr)
{
    AIRY_FREE(sr->text);
    AIRY_FREE(sr->reason);
    if (sr->tools)
        cJSON_Delete(sr->tools);
    AIRY_MEMSET(sr, 0, sizeof(*sr));
}

/* L2 呈现层按 class 分诊（0.1.19 §3.7.2 N1）：只有 transport 才允许说
 * "服务不可达"；business/policy/config/internal 必须透传原始错误码与
 * 文案，避免把工具业务失败误导为守护进程离线。 */
static char *run_fail_text(int rc)
{
    char buf[512];
    const char *s;
    if (rc == AIRY_ERR_CANCELED)
        return AIRY_STRDUP("Error: tool call canceled by user");
    if (rc == 0)
        rc = AIRY_ERR_PROTOCOL;
    s = airy_err_str(rc);
    if (!s || !s[0])
        s = "unknown error";
    if (airy_err_class_of(rc) == AIRY_ERR_CLASS_TRANSPORT)
        snprintf(buf, sizeof(buf), "Error: tool service unreachable (%s, code: %d)", s, rc);
    else
        snprintf(buf, sizeof(buf), "Error: %s (code: %d)", s, rc);
    return AIRY_STRDUP(buf);
}

/* 会话取消令牌的唯一取用点。令牌只被等待循环读取，对 const 会话取地址不
 * 破坏不可变约定；没有会话（纯执行路径）时退化为阻塞调用。 */
static airy_cancel_token_t *run_tok(const agent_run_session_t *s)
{
    return s ? (airy_cancel_token_t *)&s->cancel : NULL;
}

/* 组装 execute_tool 参数；分配失败返回 NULL。 */
static char *run_tool_params(const char *name, const char *args_json, bool allow_dangerous)
{
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return NULL;
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON_AddBoolToObject(params, "allow_dangerous", allow_dangerous);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    char *str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    return str;
}

/* 结果分诊：成败以 err_code 为准（缺失才回落 success 布尔），且「成功但产出为空」
 * 不再记成功——空正文记成功会让回执与事实脱节；业务错误透传原始 error 与码。 */
static char *run_tool_text(cJSON *result, int *out_code, int *out_ok)
{
    cJSON *err_code = cJSON_GetObjectItem(result, "err_code");
    cJSON *success = cJSON_GetObjectItem(result, "success");
    cJSON *output = cJSON_GetObjectItem(result, "output");
    cJSON *error = cJSON_GetObjectItem(result, "error");
    cJSON *ecode = cJSON_GetObjectItem(result, "error_code");
    int reported_ok = cJSON_IsNumber(err_code)
                          ? err_code->valueint == 0
                          : (cJSON_IsNumber(success) && success->valueint != 0);
    int has_output = cJSON_IsString(output) && output->valuestring && output->valuestring[0];

    *out_ok = reported_ok && has_output;
    if (reported_ok && !has_output)
        return AIRY_STRDUP("Error: tool reported success with empty output");
    if (*out_ok)
        return AIRY_STRDUP(output->valuestring);
    const char *e =
        cJSON_IsString(error) && error->valuestring ? error->valuestring : "execution failed";
    int code = cJSON_IsNumber(ecode) ? ecode->valueint : 0;
    char diag[768];
    airy_err_t ec = AIRY_ERR_UNKNOWN;
    if (airy_err_json_get(result, NULL, &ec, diag, sizeof(diag), NULL) == 0 && diag[0]) {
        e = diag;
        code = (int)ec;
    }
    char buf[1024];
    *out_code = code;
    if (code != 0)
        snprintf(buf, sizeof(buf), "Error: %s (code: %d)", e, code);
    else
        snprintf(buf, sizeof(buf), "Error: %s", e);
    return AIRY_STRDUP(buf);
}

/* 单个工具执行：tool_d.execute_tool（结果文本化，失败转 Error: 前缀）。
 * out_code 只回填工具服务自报的错误码（成功/未报则为 0）：C-16 用它识别
 * EPERM/EACCES（策略拒绝）与 ENOTSUP（无处理器），据此撤销目录条目；
 * 传输层失败不填码，避免把「服务不可达」误判成策略拒绝。
 *
 * R11：session 的取消令牌随调用下探。取消时客户端立即脱身，不等工具预算
 * 跑满；tool_d 侧按自身执行预算收尾，回执因连接关闭而丢弃，不影响审计。 */
static int run_execute_tool(const char *name, const char *args_json, char **out_text,
                            int *out_code, bool allow_dangerous, airy_cancel_token_t *tok)
{
    *out_text = NULL;
    *out_code = 0;
    char *params_str = run_tool_params(name, args_json, allow_dangerous);
    if (!params_str)
        return -1;

    char sock[AGENT_RUN_SOCK_BUF];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket("tool.sock"));
    char *resp = NULL;
    int rc = daemon_rpc_call_cancelable(sock, "execute_tool", params_str, &resp,
                                        AGENT_RUN_TOOL_TIMEOUT_MS, tok, NULL, NULL);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        AIRY_FREE(resp);
        *out_text = run_fail_text(rc);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *out_text = AIRY_STRDUP("Tool service returned invalid response");
        return -1;
    }
    /* daemon_rpc_call 已解包 JSON-RPC 外层：resp 即 {"success","err_code",
     * "output","error","exit_code","error_code"}（与 cli_chat_tools.c 同源；
     * 兼容未解包信封——有 "result" 先下钻）。 */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsObject(result))
        result = root;
    int tool_ok = 0;
    char *text = result ? run_tool_text(result, out_code, &tool_ok) : NULL;
    if (!text)
        text = AIRY_STRDUP("Tool execution returned no result");
    *out_text = text;
    cJSON_Delete(root);
    return tool_ok ? 0 : -1;
}

/* 连败计数更新（C-1 熔断）：成功清零、失败累加；返回是否达到阈值。 */
static int run_fail_streak_bump(int *streak, int tool_rc)
{
    *streak = (tool_rc == 0) ? 0 : *streak + 1;
    return *streak >= AGENT_RUN_TOOL_FAIL_LIMIT;
}

/* 调用签名：工具名与原始参数做 FNV-1a 64 位散列。整串比较需要无界缓冲，
 * 截断比较又会把长参数的不同调用误判成同一调用，故只存散列。 */
static uint64_t run_call_sig(const char *name, const char *args)
{
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = name; p && *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    h = (h ^ '|') * 1099511628211ULL;
    for (const char *p = args; p && *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    return h;
}

/* 同调用连败计数（C-14）：失败且与上次同一签名则累加，否则重置为 1；
 * 任一成功清零。返回累计次数，供换路阈值与 trace 标注使用。 */
static int run_repeat_bump(uint64_t *last, int *repeat, uint64_t sig, int tool_rc)
{
    if (tool_rc == 0) {
        *last = 0;
        *repeat = 0;
        return 0;
    }
    if (*repeat > 0 && *last == sig) {
        (*repeat)++;
    } else {
        *last = sig;
        *repeat = 1;
    }
    return *repeat;
}

/* 换路闸门（C-14）：同一调用已连续失败达到阈值时，拒绝再次以相同参数执行，
 * 把「换策略」从提示词建议升为机制约束。 */
static int run_repeat_blocked(uint64_t last, int repeat, uint64_t sig)
{
    return repeat >= AGENT_RUN_TOOL_SWITCH_REPEAT && sig == last;
}

typedef struct loop_ctx_s {
    const agent_run_loop_args_t *args;
    agent_run_loop_result_t *out;
    cJSON *messages;
    cJSON *trace;
    agent_ledger_t lg;
    char llm_sock[AGENT_RUN_SOCK_BUF];
    const char *sess;
    const char *run_id;
    uint64_t seq;
    char *text;
    char *reason;
    int streak;
    int fused;
    char fuse[256];
    uint64_t fail_sig; /* 上一次失败调用的签名（同调用换路判定） */
    int fail_repeat;   /* 同一签名的连败次数 */
    int denied;        /* D-10：本轮出现过审批/策略拒绝（失败归因用） */
    /* C-16 工具目录协商：catalog 是本轮下发给模型的条目（live 快照或
     * 静态 bootstrap），cat_denied 是本轮不可用/会话内已证不可用的工具及
     * 原因（跨轮累积，撤销后不再回到可见集）。cat_hash 记录已发射目录的
     * 指纹，目录不变则不重复推送 catalog 事件。 */
    cJSON *catalog;
    cJSON *cat_denied;
    const char *cat_src;
    uint64_t cat_hash;
    int user_cancel; /* R11：在途工具调用被用户取消，本轮立即收口 */
} loop_ctx_t;

/* 首条 system：工具路径锚点契约（缺陷 #4）声明 fs_* 与 shell_run 的相对
 * 路径以 tool_d 工作目录为基准、回执回显绝对路径；0.1.18 B1 轮次归属约束
 * 仅在携带历史时合并入同一条，防止模型把多轮历史当作同一篇待续写文本。 */
static void run_preamble(cJSON *msgs)
{
    static const char base[] =
        "【工具路径契约】文件类工具（fs_*）与 shell_run 的相对路径以工具"
        "服务（tool_d）的工作目录为解析基准；工具结果回执会回显解析后的"
        "绝对路径。";
    static const char boundary[] =
        "【轮次边界】本次请求携带了历史消息。历史消息仅供指代消解与背景"
        "理解；你的回答必须直接回应最后一条用户消息。若历史主题与最后"
        "一条用户消息不一致，以最后一条用户消息为准，禁止延续历史主题"
        "作答，禁止续写历史中未完成的回答。";
    static const char toolmake[] =
        "【自制工具】同类处理若已重复出现，优先调用 tool_create 把脚本沉淀"
        "为具名工具（脚本必须以 #! 开头，参数以 JSON 形式作为 $1 传入），"
        "它在下一轮即可按名调用，避免反复手写等价 shell 命令。";
    cJSON *sys = cJSON_CreateObject();
    if (!sys)
        return;
    char content[1024];
    snprintf(content, sizeof(content), "%s%s%s", base, toolmake,
             cJSON_GetArraySize(msgs) > 1 ? boundary : "");
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", content);
    cJSON_InsertItemInArray(msgs, 0, sys);
}

/* base64url 反解：接受无填充（与 emit 一致）也接受尾部 '='；非法字符、
 * 超容或空输入一律失败，坏令牌不得被当成有效断点。 */
static int run_unb64url(const char *src, char *dst, size_t cap)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (const char *p = src; p && *p && *p != '='; p++) {
        const char *pos = strchr(tbl, *p);
        if (!pos || o + 1 >= cap)
            return -1;
        acc = (acc << 6) | (unsigned)(pos - tbl);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    dst[o] = '\0';
    return (int)o;
}

/* 令牌解析：解 base64url 后按 JSON 对象读取，任何一步不合法即整体作废。 */
static cJSON *run_resume_parse(const char *tok)
{
    if (!tok || !tok[0])
        return NULL;
    size_t cap = strlen(tok) / 4 * 3 + 4;
    char *raw = (char *)AIRY_MALLOC(cap);
    if (!raw)
        return NULL;
    int n = run_unb64url(tok, raw, cap);
    cJSON *p = n > 0 ? cJSON_Parse(raw) : NULL;
    AIRY_FREE(raw);
    if (!p || !cJSON_IsObject(p)) {
        cJSON_Delete(p);
        return NULL;
    }
    return p;
}

/* 续跑上下文渲染：把令牌还原成一条紧凑 user 消息——原始诉求、已完成步骤
 * 摘要、待续目标。只给「做到哪、还差什么」，不复制历史消息与工具全文，
 * 这是二次续跑 token 显著低于首次全量的机制保证。 */
static char *run_seed_render(const cJSON *p)
{
    const char *prompt = cJSON_GetStringValue(cJSON_GetObjectItem(p, "prompt"));
    const char *pending = cJSON_GetStringValue(cJSON_GetObjectItem(p, "pending"));
    const cJSON *steps = cJSON_GetObjectItem(p, "steps");
    int n = cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0;
    size_t cap = 512 + (prompt ? strlen(prompt) : 0) + (pending ? strlen(pending) : 0) +
                 (size_t)n * 256;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return NULL;

    size_t o = 0;
    int w = snprintf(buf, cap, "【续跑】原始诉求：%s\n已完成步骤（不要重复执行已成功步骤）：\n",
                     prompt ? prompt : "");
    o = (w > 0 && (size_t)w < cap) ? (size_t)w : 0;
    for (int i = 0; i < n && o + 1 < cap; i++) {
        const cJSON *s = cJSON_GetArrayItem(steps, i);
        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(s, "tool"));
        const char *brief = cJSON_GetStringValue(cJSON_GetObjectItem(s, "brief"));
        const char *why = cJSON_GetStringValue(cJSON_GetObjectItem(s, "why"));
        const cJSON *ok = cJSON_GetObjectItem(s, "ok");
        w = snprintf(buf + o, cap - o, "- %s [%s] %s%s%s\n", tool ? tool : "?",
                     (cJSON_IsNumber(ok) && ok->valueint) ? "ok" : "failed", brief ? brief : "",
                     why ? " / 换路: " : "", why ? why : "");
        if (w < 0)
            break;
        o += (size_t)w < cap - o ? (size_t)w : cap - o - 1;
    }
    snprintf(buf + o, cap - o,
             "待续目标：%s\n请从上述进度继续；若同一调用再次失败，必须换参数、换工具或询问用户。",
             pending ? pending : "");
    return buf;
}

/* 首条 user 内容：令牌可用则以续跑摘要替代原始诉求；令牌损坏时退化为按原始
 * 诉求重新开始（失去断点但不阻断对话），并留下告警供排查。 */
static char *run_seed_content(loop_ctx_t *r)
{
    const char *tok = r->args->resume_token;
    if (tok && tok[0]) {
        cJSON *p = run_resume_parse(tok);
        char *c = p ? run_seed_render(p) : NULL;
        cJSON_Delete(p);
        if (c)
            return c;
        SVC_LOG_WARN("agent.run: resume_token unusable, falling back to prompt");
    }
    return AIRY_STRDUP(r->args->prompt ? r->args->prompt : "");
}

/* 初始消息集与台账入口锚点：history 非空则整体复制，否则以 prompt 构造
 * 首条 user 消息。入口记账取首条真实 user 消息——注入的 system 前置不算
 * 入口，否则被保护的是提示词样板而非用户诉求。 */
static int run_seed(loop_ctx_t *r)
{
    const cJSON *h = r->args->history;
    if (h && cJSON_IsArray(h) && cJSON_GetArraySize(h) > 0)
        r->messages = cJSON_Duplicate(h, 1);
    if (!r->messages) {
        char *content = run_seed_content(r);
        r->messages = cJSON_CreateArray();
        cJSON *m0 = r->messages ? cJSON_CreateObject() : NULL;
        if (!m0 || !content) {
            AIRY_FREE(content);
            cJSON_Delete(r->messages);
            r->messages = NULL;
            return -1;
        }
        cJSON_AddItemToArray(r->messages, m0);
        cJSON_AddStringToObject(m0, "role", "user");
        cJSON_AddStringToObject(m0, "content", content);
        AIRY_FREE(content);
    }
    run_preamble(r->messages);
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, r->messages) {
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
            agent_ledger_add(&r->lg, "user",
                             cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), 0, m);
            break;
        }
    }
    return 0;
}

/* reasoning 跨轮累积：各轮以换行连接；扩容失败保留已有内容不阻断。 */
static void run_reason_add(loop_ctx_t *r, const char *add)
{
    size_t old = r->reason ? strlen(r->reason) : 0, cap = old + strlen(add) + 2;
    char *np = (char *)AIRY_REALLOC(r->reason, cap);
    if (!np)
        return;
    r->reason = np;
    snprintf(np + old, cap - old, old > 0 ? "\n%s" : "%s", add);
}

/* 事件投递：agent_run_emit_event 自行处理无 sink / 建包失败时的载荷释放。 */
static void run_ev_send(loop_ctx_t *r, const char *type, cJSON *ev)
{
    agent_run_emit_event(r->args->sink, &r->seq, r->run_id, r->sess, type, ev);
}

/* 单轮 LLM：建参 → 流式收流 → 取产物 → 回填 assistant 消息与台账。
 * 返回 1 终局回复（正文已移交 r->text）、0 有待执行工具、-1 失败。 */
static int run_llm_round(loop_ctx_t *r, cJSON **tool_calls)
{
    *tool_calls = NULL;
    char *params = run_build_llm_params(r->args->model, r->messages, r->catalog,
                                        r->cat_src != NULL);
    if (!params)
        return -1;

    agent_stream_t st;
    run_delta_ctx_t dctx = {r->args->sink, &r->seq, r->run_id, r->sess};
    agent_stream_sink_t ssink = {run_emit_delta, &dctx};
    agent_stream_init(&st, &ssink);

    uint64_t t0 = airy_time_ms();
    int lrc = rpc_stream_idle(r->llm_sock, "complete_stream", params, agent_stream_on_chunk, &st,
                              AGENT_RUN_LLM_TIMEOUT_MS, AGENT_RUN_LLM_TOTAL_MS);
    r->out->llm_ms += airy_time_ms() - t0;
    AIRY_FREE(params);
    agent_stream_finish(&st);

    agent_stream_result_t sr;
    agent_stream_take(&st, &sr);
    agent_stream_free(&st);
    if (lrc != AIRY_SUCCESS || sr.error_code != 0) {
        /* 上游失败必须带着原始码与文案离开本函数：以前只记日志并返回 -1，
         * 终态帧只能兜底成「tool loop exhausted or LLM service error」，
         * 客户端把它渲染成「未知错误」。 */
        int ec = sr.error_code ? sr.error_code : lrc;
        const char *detail = sr.error_msg[0] ? sr.error_msg : airy_err_str(ec);
        int transport = ec > 0 && airy_err_class_of(ec) == AIRY_ERR_CLASS_TRANSPORT;
        if (!detail || !detail[0])
            detail = "no detail from provider";
        r->out->fail_transport = transport;
        if (transport)
            snprintf(r->out->fail_msg, sizeof(r->out->fail_msg),
                     "LLM service unreachable (code=%d): %s", ec, detail);
        else
            snprintf(r->out->fail_msg, sizeof(r->out->fail_msg),
                     "LLM request failed (code=%d): %s", ec, detail);
        SVC_LOG_ERROR("agent.run: llm stream failed (rc=%d, ec=%d, msg=%s, session=%s)", lrc,
                      sr.error_code, sr.error_msg, r->sess ? r->sess : "?");
        run_stream_result_free(&sr);
        return AGENT_RUN_RC_LLM_FAIL;
    }

    r->out->tokens += sr.tokens;
    r->out->tokens_in += sr.prompt_tokens;
    r->out->tokens_out += sr.completion_tokens;
    r->out->cost += sr.cost;
    if (sr.reason && sr.reason[0])
        run_reason_add(r, sr.reason);
    AIRY_FREE(sr.reason);

    /* 空数组与「无 tool_calls」同义（旧解析器同判）：本轮即终局回复。 */
    cJSON *calls = sr.tools;
    if (calls && cJSON_GetArraySize(calls) == 0) {
        cJSON_Delete(calls);
        calls = NULL;
    }
    cJSON *am = cJSON_CreateObject();
    cJSON_AddStringToObject(am, "role", "assistant");
    cJSON_AddStringToObject(am, "content", sr.text ? sr.text : "");
    if (calls)
        cJSON_AddItemToObject(am, "tool_calls", cJSON_Duplicate(calls, 1));
    cJSON_AddItemToArray(r->messages, am);
    agent_ledger_add(&r->lg, "assistant", sr.text ? sr.text : "", (size_t)sr.tokens, am);

    if (calls) {
        AIRY_FREE(sr.text);
        *tool_calls = calls;
        return 0;
    }
    r->text = sr.text;
    return 1;
}

/* ---- C-16 工具目录协商 ----
 * 目录唯一来源是 tool_d 的 list_tools（含 available / denied_reason）：本
 * 模块每轮采样一次（≤1 次），同一份快照既用于下发 LLM，也用于派发校验，
 * 消除「下发一份、执行用另一份」。会话内被证明不可用（策略拒绝 / 无处理器）
 * 的工具进 cat_denied 并跨轮累积，重新采样时不再回到可见集。 */

static int run_cat_has(const cJSON *tools, const char *name)
{
    const cJSON *it = NULL;
    if (!tools || !name || !name[0])
        return 0;
    cJSON_ArrayForEach(it, tools)
    {
        const char *n =
            cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(it, "function"), "name"));
        if (n && strcmp(n, name) == 0)
            return 1;
    }
    return 0;
}

/* 同名条目全部移除：重定义（tool_create 后重新采样）与撤销共用此出口。 */
static void run_cat_drop(cJSON *tools, const char *name)
{
    if (!tools || !name || !name[0])
        return;
    for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; --i) {
        const char *n = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(tools, i), "function"),
                                "name"));
        if (n && strcmp(n, name) == 0)
            cJSON_DeleteItemFromArray(tools, i);
    }
}

/* 追加一条 OpenAI tools 条目；schema 缺失时补合法空对象，避免模型侧
 * 因单个条目不合法而拒收整套工具。 */
static void run_cat_add(cJSON *tools, const char *name, const char *desc, const cJSON *schema)
{
    if (!tools || !name || !name[0])
        return;
    run_cat_drop(tools, name);
    cJSON *entry = cJSON_CreateObject();
    cJSON *fn = cJSON_CreateObject();
    if (!entry || !fn) {
        cJSON_Delete(entry);
        cJSON_Delete(fn);
        return;
    }
    cJSON_AddStringToObject(entry, "type", "function");
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "description", desc ? desc : "");
    cJSON *params = cJSON_IsObject(schema) ? cJSON_Duplicate(schema, 1) : NULL;
    if (!params) {
        params = cJSON_CreateObject();
        if (params) {
            cJSON_AddStringToObject(params, "type", "object");
            cJSON_AddItemToObject(params, "properties", cJSON_CreateObject());
            cJSON_AddItemToObject(params, "required", cJSON_CreateArray());
        }
    }
    cJSON_AddItemToObject(fn, "parameters", params);
    cJSON_AddItemToObject(entry, "function", fn);
    cJSON_AddItemToArray(tools, entry);
}

static int run_cat_denied_has(const cJSON *denied, const char *name)
{
    const cJSON *it = NULL;
    if (!denied || !name || !name[0])
        return 0;
    cJSON_ArrayForEach(it, denied)
    {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(it, AIRY_RS_K_TOOL));
        if (n && strcmp(n, name) == 0)
            return 1;
    }
    return 0;
}

static const char *cat_block(const cJSON *denied, const char *name)
{
    const cJSON *it = NULL;
    if (!denied || !name)
        return NULL;
    cJSON_ArrayForEach(it, denied)
    {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(it, AIRY_RS_K_TOOL));
        if (n && strcmp(n, name) == 0)
            return cJSON_GetStringValue(cJSON_GetObjectItem(it, AIRY_RS_K_DENIED_REASON));
    }
    return NULL;
}

/* 记录不可用原因：同一工具在会话内保留首个原因，不因后续调用改写。 */
static void run_cat_deny(cJSON *denied, const char *name, const char *reason)
{
    if (!denied || !name || !name[0] || run_cat_denied_has(denied, name))
        return;
    cJSON *e = cJSON_CreateObject();
    if (!e)
        return;
    cJSON_AddStringToObject(e, AIRY_RS_K_TOOL, name);
    cJSON_AddStringToObject(e, AIRY_RS_K_DENIED_REASON,
                            reason && reason[0] ? reason : AIRY_TOOL_DENIED_POLICY);
    cJSON_AddItemToArray(denied, e);
}

/* 目录快照事件：tools 是本轮真正下发给模型的工具名，available 是可用条数，
 * denied 逐条带原因。catalog_source 区分实时目录与静态 bootstrap。 */
static void run_cat_emit(loop_ctx_t *r)
{
    if (!r->catalog || !r->cat_src)
        return;
    cJSON *ev = cJSON_CreateObject();
    if (!ev)
        return;
    cJSON_AddStringToObject(ev, AIRY_RS_K_CATALOG_SOURCE, r->cat_src);
    cJSON *names = cJSON_CreateArray();
    int avail = 0;
    if (names) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, r->catalog)
        {
            const char *n = cJSON_GetStringValue(
                cJSON_GetObjectItem(cJSON_GetObjectItem(it, "function"), "name"));
            if (n) {
                cJSON_AddItemToArray(names, cJSON_CreateString(n));
                avail++;
            }
        }
        cJSON_AddItemToObject(ev, AIRY_RS_K_TOOLS, names);
    }
    cJSON_AddNumberToObject(ev, AIRY_RS_K_AVAILABLE, avail);
    cJSON *den = r->cat_denied ? cJSON_Duplicate(r->cat_denied, 1) : NULL;
    cJSON_AddItemToObject(ev, AIRY_RS_K_DENIED, den ? den : cJSON_CreateArray());
    run_ev_send(r, AIRY_RS_TYPE_CATALOG, ev);
}

/* 目录指纹：来源 + 可见集 + 不可用集。指纹不变说明目录没变，不重复推送。 */
static uint64_t run_cat_sig(const loop_ctx_t *r)
{
    uint64_t h = run_call_sig(r->cat_src ? r->cat_src : "", "");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, r->catalog)
    {
        const char *n =
            cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(it, "function"), "name"));
        if (n)
            h = run_call_sig(n, "") ^ (h * 1099511628211ULL);
    }
    cJSON_ArrayForEach(it, r->cat_denied)
    {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(it, AIRY_RS_K_TOOL));
        const char *rs =
            cJSON_GetStringValue(cJSON_GetObjectItem(it, AIRY_RS_K_DENIED_REASON));
        if (n)
            h = run_call_sig(n, rs ? rs : "") ^ (h * 1099511628211ULL);
    }
    return h;
}

/* tool_d 不可达时的 bootstrap：静态 SSoT 只保证模型仍有工具可用，事件必须
 * 标 catalog_source=static（判据允许的唯一例外，且必须对客户端可见）。
 * 会话内已撤销的工具在 bootstrap 中同样剔除。 */
static void run_catalog_static(loop_ctx_t *r)
{
    cJSON *src = cJSON_Parse(AIRY_TOOLS_JSON_SOURCE);
    if (!src)
        return;
    for (int i = cJSON_GetArraySize(src) - 1; i >= 0; --i) {
        const char *n = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(src, i), "function"), "name"));
        if (n && run_cat_denied_has(r->cat_denied, n))
            cJSON_DeleteItemFromArray(src, i);
    }
    cJSON_Delete(r->catalog);
    r->catalog = src;
    r->cat_src = AIRY_RS_CATALOG_SOURCE_STATIC;
}

/* 每轮唯一的一次目录采样：include_denied 一次取全，避免两次调用产生两份
 * 不一致快照；不可用条目在此分流进 cat_denied，绝不进 catalog。 */
static void run_catalog_load(loop_ctx_t *r)
{
    if (!r->catalog || !r->cat_denied)
        return;
    char sock[AGENT_RUN_SOCK_BUF];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket("tool.sock"));
    char *resp = NULL;
    int rc = daemon_rpc_call(sock, "list_tools", "{\"include_denied\":true}", &resp,
                             AGENT_RUN_TOOL_TIMEOUT_MS);
    cJSON *root = NULL;
    if (rc == AIRY_SUCCESS && resp)
        root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    cJSON *list = root ? (cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "tools")) : NULL;
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        run_catalog_static(r);
        uint64_t sig = run_cat_sig(r);
        if (sig != r->cat_hash) {
            r->cat_hash = sig;
            run_cat_emit(r);
        }
        return;
    }

    cJSON *next = cJSON_CreateArray();
    if (!next) {
        cJSON_Delete(root);
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, list)
    {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        if (!n || !n[0])
            n = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        if (!n || !n[0])
            continue;
        cJSON *avail = cJSON_GetObjectItem(it, "available");
        if (cJSON_IsBool(avail) && !cJSON_IsTrue(avail)) {
            run_cat_deny(r->cat_denied, n,
                         cJSON_GetStringValue(cJSON_GetObjectItem(it, AIRY_RS_K_DENIED_REASON)));
            continue;
        }
        if (run_cat_denied_has(r->cat_denied, n))
            continue; /* 会话内已证不可用：不再回到可见集 */
        run_cat_add(next, n, cJSON_GetStringValue(cJSON_GetObjectItem(it, "description")),
                    cJSON_GetObjectItem(it, "input_schema"));
    }
    cJSON_Delete(root);

    cJSON_Delete(r->catalog);
    r->catalog = next;
    r->cat_src = AIRY_RS_CATALOG_SOURCE_LIVE;

    uint64_t sig = run_cat_sig(r);
    if (sig != r->cat_hash) {
        r->cat_hash = sig;
        run_cat_emit(r);
    }
}

/* 工具自报策略拒绝 / 无处理器：立即移出本轮目录并记原因，下一轮重新采样
 * 时不再回到可见集。传输失败与业务错误（参数非法等）不撤销——那是调用
 * 问题，不是工具不可用。 */
static void run_cat_revoke(loop_ctx_t *r, const char *name, int code)
{
    const char *reason = NULL;
    switch (code) {
    case AIRY_ERR_PERMISSION_DENIED:
    case AIRY_EPERM:
    case AIRY_EACCES:
        reason = AIRY_TOOL_DENIED_POLICY;
        break;
    case AIRY_ERR_NOT_SUPPORTED:
    case AIRY_ENOTSUP:
        reason = AIRY_TOOL_DENIED_HANDLER;
        break;
    default:
        return;
    }
    if (!name || !name[0])
        return;
    run_cat_deny(r->cat_denied, name, reason);
    run_cat_drop(r->catalog, name);
    r->cat_hash = 0; /* 目录已变：下一轮必须重新推送快照事件 */
}

/* 定长字段装填：先按容量拷贝，再把末尾可能切半的多字节字符退掉。
 * 链事件与 trace 的单字段是固定 160 字节，字节级截断会把 UTF-8 切坏，
 * 下游按 JSON 严格解析时整条事件作废。 */
static void run_fit(char *buf, size_t cap, const char *src)
{
    AIRY_STRNCPY_TERM(buf, src ? src : "", cap);
    buf[utf8_prefix_len(buf, strlen(buf))] = '\0';
}

/* base64url（无填充）：令牌会出现在 JSON 字段与 URL query 中，'+' '/' '='
 * 必须避开，否则调用方还得再转义一层。 */
static char *run_b64url(const unsigned char *src, size_t n)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char *out = (char *)AIRY_MALLOC((n + 2) / 3 * 4 + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)src[i] << 16;
        if (i + 1 < n)
            v |= (unsigned)src[i + 1] << 8;
        if (i + 2 < n)
            v |= src[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        if (i + 1 < n)
            out[o++] = tbl[(v >> 6) & 63];
        if (i + 2 < n)
            out[o++] = tbl[v & 63];
    }
    out[o] = '\0';
    return out;
}

/* 已完成步骤摘要：只保留最近 keep 条，字段压到最小（工具名、成败、结果首
 * 段、换路原因），避免令牌膨胀成第二份历史。 */
static cJSON *run_resume_steps(const cJSON *trace, int keep)
{
    cJSON *steps = cJSON_CreateArray();
    if (!steps)
        return NULL;
    int n = cJSON_GetArraySize(trace);
    int start = n > keep ? n - keep : 0;
    for (int i = start; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(trace, i);
        const cJSON *tool = cJSON_GetObjectItem(it, "tool");
        const cJSON *ok = cJSON_GetObjectItem(it, "ok");
        const cJSON *res = cJSON_GetObjectItem(it, "result");
        const cJSON *sr = cJSON_GetObjectItem(it, "switch_reason");
        cJSON *s = cJSON_CreateObject();
        if (!s)
            break;
        char brief[80];
        run_fit(brief, sizeof(brief), cJSON_IsString(res) ? res->valuestring : "");
        cJSON_AddStringToObject(s, "tool", cJSON_IsString(tool) ? tool->valuestring : "");
        cJSON_AddNumberToObject(s, "ok", (cJSON_IsNumber(ok) && ok->valueint != 0) ? 1 : 0);
        cJSON_AddStringToObject(s, "brief", brief);
        if (cJSON_IsString(sr)) {
            char why[80];
            run_fit(why, sizeof(why), sr->valuestring);
            cJSON_AddStringToObject(s, "why", why);
        }
        cJSON_AddItemToArray(steps, s);
    }
    return steps;
}

/* 续跑令牌载荷：做到哪（步骤摘要）+ 还差什么（待续正文）+ 原始诉求与计量。
 * 服务端无状态，客户端凭令牌原地续跑，无需重发全量历史。 */
static char *run_resume_pack(const loop_ctx_t *r, int keep)
{
    cJSON *p = cJSON_CreateObject();
    if (!p)
        return NULL;
    char prompt[AGENT_RUN_RESUME_TEXT_MAX + 1];
    char pending[AGENT_RUN_RESUME_TEXT_MAX + 1];
    run_fit(prompt, sizeof(prompt), r->args->prompt ? r->args->prompt : "");
    run_fit(pending, sizeof(pending), r->text ? r->text : "");
    cJSON_AddNumberToObject(p, "v", 1);
    cJSON_AddStringToObject(p, "sess", r->sess ? r->sess : "");
    cJSON_AddStringToObject(p, "prompt", prompt);
    cJSON_AddStringToObject(p, "pending", pending);
    cJSON *steps = run_resume_steps(r->trace, keep);
    if (steps)
        cJSON_AddItemToObject(p, "steps", steps);
    cJSON_AddNumberToObject(p, "used", r->out->rounds_used);
    cJSON_AddNumberToObject(p, "limit", r->out->rounds_limit);

    char *json = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    if (!json)
        return NULL;
    char *tok = run_b64url((const unsigned char *)json, strlen(json));
    AIRY_FREE(json);
    return tok;
}

/* 续跑令牌生成（C-14）：成品受 TOKEN_MAX 硬约束。步骤摘要可裁剪，超限时
 * 先砍步骤数量再重编；砍到零仍放不下才放弃令牌（正文与 trace 不受影响）。 */
static char *run_resume_emit(const loop_ctx_t *r)
{
    int keep = cJSON_GetArraySize(r->trace);
    if (keep > AGENT_RUN_RESUME_STEP_MAX)
        keep = AGENT_RUN_RESUME_STEP_MAX;
    for (;;) {
        char *tok = run_resume_pack(r, keep);
        if (tok && strlen(tok) <= AGENT_RUN_RESUME_TOKEN_MAX)
            return tok;
        AIRY_FREE(tok);
        if (keep == 0)
            return NULL;
        keep /= 2;
    }
}

/* C-25：主工具失败后的等价换路。换路是同一次调用的处置而非独立重试预算：
 * 执行时长并入同一次 tool_ms，连败/熔断仍按一次失败记账；每步至多换一次，
 * 替代工具失败后不再连锁换路（避免把一次失败放大成一次搜索）。策略拒绝类
 * 错误码由调用方过滤，替代工具必须在当前目录内才执行。 */
typedef int (*run_exec_fn)(const char *name, const char *args, char **out, int *code,
                           bool allow_dangerous, airy_cancel_token_t *tok);

static int run_alt_try(loop_ctx_t *r, run_alt_t *alt, run_exec_fn exec, char **out, cJSON **fbev)
{
    char fit[160];
    run_fit(fit, sizeof(fit), alt->args);
    cJSON *ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL, alt->tool);
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL_ID, "");
        cJSON_AddStringToObject(ev, AIRY_RS_K_ARGS, fit);
    }
    run_ev_send(r, AIRY_RS_TYPE_TOOL_START, ev);

    char *res = NULL;
    int code = 0;
    uint64_t t0 = airy_time_ms();
    int rc = exec(alt->tool, alt->args, &res, &code, r->args->allow_dangerous,
                  run_tok(r->args->session));
    r->out->tool_ms += airy_time_ms() - t0;
    if (rc != 0 && code != 0)
        run_cat_revoke(r, alt->tool, code);

    run_fit(fit, sizeof(fit), res ? res : "");
    ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL, alt->tool);
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL_ID, "");
        cJSON_AddStringToObject(ev, AIRY_RS_K_STATUS, rc == 0 ? "ok" : "error");
        cJSON_AddStringToObject(ev, AIRY_RS_K_RESULT_HASH, fit);
    }
    run_ev_send(r, AIRY_RS_TYPE_TOOL_END, ev);

    if (fbev) {
        cJSON *fb = cJSON_CreateObject();
        if (fb) {
            cJSON_AddStringToObject(fb, "tool", alt->tool);
            cJSON_AddStringToObject(fb, "note", alt->note);
            cJSON_AddNumberToObject(fb, "ok", rc == 0 ? 1 : 0);
            *fbev = fb;
        }
    }
    if (rc == 0) {
        AIRY_FREE(*out);
        *out = res;
        return 0;
    }
    /* 换路也失败：回填两段原因并明确「表内已无替代」，让模型下一步只能
     * 换参数或询问用户——这是可解释终止，不是静默重试。 */
    char both[768];
    snprintf(both, sizeof(both),
             "Error: %s failed and its equivalent %s also failed (%s). No further equivalent tool "
             "is available: change the arguments or ask the user.",
             *out ? *out : "", alt->tool, res ? res : "");
    AIRY_FREE(*out);
    AIRY_FREE(res);
    *out = AIRY_STRDUP(both);
    return -1;
}

/* 单次工具调用的中间态：让「解析 / 拒绝 / 执行 / 记账 / 落盘」各自成函数，
 * 避免单函数承担全部分支（R18 复杂度压降）。 */
typedef struct {
    const char *tname;
    const char *targs;
    const char *tid;
    uint64_t sig;
    int blocked;
    int erc;
    char *res;
    cJSON *fbev;
    char note[256];
    const char *why; /* 换路原因：指向 note 或 NULL */
} run_tc_t;

/* 解析本次调用并广播 tool_start，随后算出调用签名与重复闸门。 */
static void run_tc_head(loop_ctx_t *r, run_tc_t *c, cJSON *tc)
{
    cJSON *fn = cJSON_GetObjectItem(tc, "function");
    cJSON *fname = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
    cJSON *fargs = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
    cJSON *tcid = cJSON_GetObjectItem(tc, "id");
    char buf[160];

    c->tname = cJSON_IsString(fname) ? fname->valuestring : "";
    c->targs = cJSON_IsString(fargs) ? fargs->valuestring : "{}";
    c->tid = cJSON_IsString(tcid) ? tcid->valuestring : "";
    run_fit(buf, sizeof(buf), c->targs);
    cJSON *ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL, c->tname);
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL_ID, c->tid);
        cJSON_AddStringToObject(ev, AIRY_RS_K_ARGS, buf);
    }
    run_ev_send(r, AIRY_RS_TYPE_TOOL_START, ev);
    c->sig = run_call_sig(c->tname, c->targs);
    c->blocked = run_repeat_blocked(r->fail_sig, r->fail_repeat, c->sig);
}

/* C-16：派发面与下发面同源。模型可能凭历史记忆调用本轮目录外的工具
 * （已撤销或不存在的），这里不执行并回填可行动原因。 */
static void run_tc_miss(loop_ctx_t *r, run_tc_t *c)
{
    const char *why = cat_block(r->cat_denied, c->tname);

    c->erc = -1;
    snprintf(c->note, sizeof(c->note),
             "Error: tool \"%s\" is not available in this run's catalog (reason: %s); "
             "choose a tool from the current tool list or ask the user.",
             c->tname, why ? why : "not_listed");
    c->res = AIRY_STRDUP(c->note);
    snprintf(c->note, sizeof(c->note), "tool %s not in catalog; dispatch refused", c->tname);
    c->why = c->note;
}

/* 同一调用已连败两次：不再执行，换路要求直接作为工具结果回传，模型下一轮
 * 必须换参数、换工具或询问用户，而不是原样重试。 */
static void run_tc_block(loop_ctx_t *r, run_tc_t *c)
{
    c->erc = -1;
    snprintf(c->note, sizeof(c->note),
             "Error: identical call to %s rejected after %d consecutive failures with the "
             "same arguments; change the arguments, switch tools, or ask the user before "
             "retrying.",
             c->tname, r->fail_repeat);
    c->res = AIRY_STRDUP(c->note);
    snprintf(c->note, sizeof(c->note),
             "identical call %s blocked after %d failures; strategy switch required", c->tname,
             r->fail_repeat);
    c->why = c->note;
}

/* R11：在途取消。结果已无意义，以 canceled 收尾本轮，由外层按取消终止整次 run。 */
static void run_tc_cancel(loop_ctx_t *r, run_tc_t *c)
{
    cJSON *ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL_ID, c->tid);
        cJSON_AddStringToObject(ev, AIRY_RS_K_STATUS, "canceled");
        cJSON_AddStringToObject(ev, AIRY_RS_K_RESULT_HASH, "");
    }
    run_ev_send(r, AIRY_RS_TYPE_TOOL_END, ev);
}

/* 真实执行 + 目录撤销 + C-25 等价换路；返回 1 表示已按取消终止（结果不入账）。 */
static int run_tc_exec(loop_ctx_t *r, run_tc_t *c)
{
    int code = 0;
    uint64_t t0 = airy_time_ms();

    c->erc = run_execute_tool(c->tname, c->targs, &c->res, &code, r->args->allow_dangerous,
                              run_tok(r->args->session));
    r->out->tool_ms += airy_time_ms() - t0;
    if (r->args->session && agent_run_is_cancelled(r->args->session)) {
        r->user_cancel = 1;
        run_tc_cancel(r, c);
        AIRY_FREE(c->res);
        c->res = NULL;
        return 1;
    }
    /* 工具自报不可用（策略拒绝 / 无处理器）：撤销目录条目并记原因。
     * tool_create 的新工具不必在此追加，下一轮目录采样自然可见。 */
    if (c->erc != 0 && code != 0) {
        /* D-10：审批/策略拒绝单独留痕，失败归因可区分「工具坏了」与「人没批」。 */
        if (code == AIRY_ERR_PERMISSION_DENIED || code == AIRY_EPERM || code == AIRY_EACCES)
            r->denied = 1;
        run_cat_revoke(r, c->tname, code);
    }
    /* C-25：失败且不属策略拒绝时，按等价表换路一次；开关关闭则保持旧语义
     * （只计数）。无替代时把原因写进 why，复盘可见。 */
    if (c->erc == 0 || run_alt_deny(code) || !run_alt_enabled())
        return 0;
    run_alt_t alt;
    if (run_alt_plan(c->tname, c->targs, &alt) && run_cat_has(r->catalog, alt.tool)) {
        c->erc = run_alt_try(r, &alt, run_execute_tool, &c->res, &c->fbev);
        snprintf(c->note, sizeof(c->note), "%s failed; switched to %s", c->tname, alt.tool);
        c->why = c->note;
    } else {
        snprintf(c->note, sizeof(c->note),
                 "no equivalent tool for %s; arguments must change or the user must be asked",
                 c->tname);
        c->why = c->note;
    }
    run_alt_free(&alt);
    return 0;
}

/* C-1 连败计数：失败仍完整入账/推送后再判断熔断出口。被闸门拒绝的调用
 * 尚未真正执行，不计入熔断，避免替模型把 run 直接判死。 */
static void run_tc_acct(loop_ctx_t *r, run_tc_t *c)
{
    if (!c->blocked && run_fail_streak_bump(&r->streak, c->erc)) {
        r->fused = 1;
        snprintf(r->fuse, sizeof(r->fuse), "%s: %s", c->tname, c->res ? c->res : "");
        r->fuse[utf8_prefix_len(r->fuse, strlen(r->fuse))] = '\0';
    }
    if (!c->blocked)
        run_repeat_bump(&r->fail_sig, &r->fail_repeat, c->sig, c->erc);
    if (!c->why && c->erc != 0 && r->fail_repeat == AGENT_RUN_TOOL_SWITCH_REPEAT) {
        /* 第二次相同失败：本步即标注原因，复盘能看出下一步为何被拒。 */
        snprintf(c->note, sizeof(c->note),
                 "identical call %s failed %d times in a row; next identical call will be rejected",
                 c->tname, r->fail_repeat);
        c->why = c->note;
    }
}

/* 回填 messages / 台账 / trace（后者供最终响应），并在工具边界落检查点。
 * 回填预算（C-23）：长输出按 8 KB 头尾保留，超出部分以标记替代，避免整段
 * 塞回上下文挤掉后续轮次；预算与 UTF-8 边界由 string SSoT 统一裁决。 */
static void run_tc_rec(loop_ctx_t *r, run_tc_t *c)
{
    char buf[160];
    cJSON *ev;

    AIRY_STRNCPY_TERM(buf, c->res ? c->res : "", sizeof(buf));
    buf[utf8_prefix_len(buf, strlen(buf))] = '\0';
    ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL_ID, c->tid);
        cJSON_AddStringToObject(ev, AIRY_RS_K_STATUS, c->erc == 0 ? "ok" : "error");
        cJSON_AddStringToObject(ev, AIRY_RS_K_RESULT_HASH, buf);
    }
    run_ev_send(r, AIRY_RS_TYPE_TOOL_END, ev);

    char *content = airy_text_budget(c->res ? c->res : "Tool execution failed", 0);
    if (!content)
        content = AIRY_STRDUP("Tool execution failed");

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", c->tid);
    cJSON_AddStringToObject(msg, "content", content ? content : "Tool execution failed");
    cJSON_AddItemToArray(r->messages, msg);
    agent_ledger_add(&r->lg, "tool_result", content, 0, msg);

    cJSON *tr = cJSON_CreateObject();
    cJSON_AddStringToObject(tr, "tool", c->tname);
    cJSON_AddStringToObject(tr, "arguments", c->targs);
    cJSON_AddStringToObject(tr, "result", content ? content : "");
    cJSON_AddNumberToObject(tr, "ok", c->erc == 0 ? 1 : 0);
    if (c->why)
        cJSON_AddStringToObject(tr, "switch_reason", c->why);
    if (c->fbev)
        cJSON_AddItemToObject(tr, "fallback", c->fbev);
    cJSON_AddItemToArray(r->trace, tr);

    /* D-12：工具边界即检查点。令牌只含边界前的步骤摘要，守护重启后客户端
     * 凭 agent.turns 取回令牌续跑，不会重复执行已完成的工具。 */
    if (r->out->rounds_used > 0) {
        char *tok = run_resume_emit(r);
        if (tok) {
            const char *key = r->run_id ? r->run_id : r->sess;
            if (key && key[0])
                agent_ckpt_save(key, r->sess, r->out->rounds_used, r->out->rounds_limit,
                                cJSON_GetArraySize(r->trace), c->tname, tok);
            AIRY_FREE(tok);
        }
    }
    AIRY_FREE(content);
    AIRY_FREE(c->res);
    c->res = NULL;
}

/* 单条工具调用：tool_start → 执行 → 连败计数 → tool_end → 回填 messages /
 * 台账 / trace（后者供最终响应）。返回熔断是否触发。 */
static int run_tool_one(loop_ctx_t *r, cJSON *tc)
{
    run_tc_t c = {0};

    run_tc_head(r, &c, tc);
    if (!run_cat_has(r->catalog, c.tname))
        run_tc_miss(r, &c);
    else if (c.blocked)
        run_tc_block(r, &c);
    else if (run_tc_exec(r, &c))
        return r->fused;
    run_tc_acct(r, &c);
    run_tc_rec(r, &c);
    return r->fused;
}

/* 单轮推进：一轮 LLM 收流 + 本轮全部工具调用。返回 1 继续下一轮、0 终局
 * 回复已就绪（正文在 r->text）、-1 失败、熔断返回 AGENT_RUN_RC_TOOL_FUSE
 * （收尾文案写入 r->text）。 */
static int run_step(loop_ctx_t *r)
{
    cJSON *calls = NULL;
    char msg[384];
    int st = run_llm_round(r, &calls);
    /* run_llm_round：1 终局正文、0 有待执行工具、其余为终局失败码
     * （-1 内部错误 / AGENT_RUN_RC_LLM_FAIL 上游）。失败码原样上抛，
     * 不可压成 -1，否则错误明细与分类在终态帧处丢失。 */
    if (st == 1)
        return 0;
    if (st != 0)
        return st;

    int n = cJSON_GetArraySize(calls);
    for (int k = 0; k < n && !r->fused && !r->user_cancel; k++)
        run_tool_one(r, cJSON_GetArrayItem(calls, k));
    cJSON_Delete(calls);
    /* R11：取消不是故障。返回 1 让主循环在下一轮起点按取消收口，
     * 不记熔断、不产出失败载荷。 */
    if (r->user_cancel || !r->fused)
        return 1;

    /* C-1 熔断终局：止损退出（不再消耗 LLM 轮次），原因经 text 回传，
     * engine 侧 error 事件与 response 复用。 */
    snprintf(msg, sizeof(msg),
             "Tool execution failed %d times in a row; further attempts stopped (%s)",
             r->streak, r->fuse);
    r->text = AIRY_STRDUP(msg);
    return AGENT_RUN_RC_TOOL_FUSE;
}

/* 运行态初始化：绑定参数/产物并装配消息、目录、拒绝表与种子。 */
static int run_loop_init(loop_ctx_t *r, const agent_run_loop_args_t *args,
                         agent_run_loop_result_t *out)
{
    AIRY_MEMSET(r, 0, sizeof(*r));
    AIRY_MEMSET(out, 0, sizeof(*out));
    r->args = args;
    r->out = out;
    r->sess = args->session ? args->session->session_id : NULL;
    r->run_id = args->session ? args->session->run_id : NULL;
    snprintf(r->llm_sock, sizeof(r->llm_sock), "%s", airy_runtime_dir_socket("llm.sock"));
    agent_ledger_init(&r->lg, r->sess);

    r->trace = cJSON_CreateArray();
    r->catalog = cJSON_CreateArray();
    r->cat_denied = cJSON_CreateArray();
    if (r->trace && r->catalog && r->cat_denied && run_seed(r) == 0)
        return 0;
    cJSON_Delete(r->messages);
    cJSON_Delete(r->trace);
    cJSON_Delete(r->catalog);
    cJSON_Delete(r->cat_denied);
    return -1;
}

/* 主轮次推进；返回终局码，*more 表示预算耗尽时模型仍在推进。 */
static int run_loop_rounds(loop_ctx_t *r, agent_run_loop_result_t *out, int limit, int *used,
                           int *more, int *cancelled)
{
    int rc = -1;

    for (int i = 0; i < limit; i++) {
        *used = i + 1;
        /* 边界检查点与令牌复用同一轮次口径：先更新产物再进轮。 */
        out->rounds_used = *used;
        if (agent_run_is_cancelled(r->args->session)) {
            SVC_LOG_INFO("agent.run: cancelled by user (session=%s)", r->sess ? r->sess : "?");
            rc = 1;
            *cancelled = 1;
            *more = 0;
            break;
        }
        agent_ledger_fit(&r->lg, r->messages);

        /* C-16：每轮采样一次工具目录（≤1 次 list_tools），失败回落静态
         * bootstrap 并以 catalog_source=static 事件对客户端可见。 */
        run_catalog_load(r);
        int st = run_step(r);
        if (st != 1) {
            rc = st;
            *more = 0;
            break;
        }
        *more = 1;
    }
    return rc;
}

/* affected_node 只留工具 id；附带错误文本由 trace 承载。 */
static void run_loop_fuse(const loop_ctx_t *r, agent_run_loop_result_t *out)
{
    if (!r->fused)
        return;
    const char *sep = strchr(r->fuse, ':');
    size_t n = sep ? (size_t)(sep - r->fuse) : strlen(r->fuse);
    if (n >= sizeof(out->fail_node))
        n = sizeof(out->fail_node) - 1;
    AIRY_MEMCPY(out->fail_node, r->fuse, n);
    out->fail_node[n] = '\0';
}

/* D-12：终态收口。暂停（含预算耗尽与可重试的 provider 故障）保留可续跑
 * 条目并写入含已执行步骤的令牌；其余终态删除条目。退出排水期由检查点
 * 模块保留为 paused。 */
static void run_loop_ckpt(loop_ctx_t *r, agent_run_loop_result_t *out, int cancelled, int rc)
{
    const char *key = r->run_id ? r->run_id : r->sess;
    bool paused;

    if (!key || !key[0])
        return;
    paused = !cancelled && rc != 0 && rc != AGENT_RUN_RC_UNMET &&
             rc != AGENT_RUN_RC_TOOL_FUSE && rc != AGENT_RUN_RC_UNVERIFIED;
    if (paused && !out->resume_token) {
        /* 故障暂停同样要能把最后一个工具边界交还客户端，否则只能重跑。 */
        out->resume_token = run_resume_emit(r);
    }
    if (paused && out->resume_token) {
        agent_ckpt_save(key, r->sess, out->rounds_used, out->rounds_limit,
                        cJSON_GetArraySize(r->trace), "", out->resume_token);
        agent_ckpt_pend(key);
    } else {
        agent_ckpt_done(key);
    }
}

/* 终局码（含取消与预算耗尽）移交产物，失败路径就地释放；计量已直写 out。 */
static int run_loop_fin(loop_ctx_t *r, agent_run_loop_result_t *out, int rc)
{
    agent_ledger_free(&r->lg);
    cJSON_Delete(r->messages);
    cJSON_Delete(r->catalog);
    cJSON_Delete(r->cat_denied);
    if (rc >= 0 || rc == AIRY_EBUDGET) {
        out->trace = r->trace;
        out->text = r->text;
        out->reason = r->reason;
        return rc;
    }
    cJSON_Delete(r->trace);
    AIRY_FREE(r->text);
    AIRY_FREE(r->reason);
    return rc;
}

int agent_run_tool_loop(const agent_run_loop_args_t *args, agent_run_loop_result_t *out)
{
    loop_ctx_t r;
    int rc;
    int more = 0;
    int cancelled = 0;
    int used = 0;

    if (!args || !out)
        return -1;
    if (run_loop_init(&r, args, out) != 0)
        return -1;

    const int limit = rounds_resolve(args->rounds);
    out->rounds_limit = limit;
    rc = run_loop_rounds(&r, out, limit, &used, &more, &cancelled);
    if (rc == AGENT_RUN_RC_TOOL_FUSE && r.fused)
        run_loop_fuse(&r, out);
    out->fail_denied = r.denied;

    out->rounds_used = used;
    /* 轮次预算耗尽不是崩溃：模型仍在推进（每轮都在调用工具），只是未收敛。
     * 如实给出终局码与可读原因，并令 trace 随结果移交——此前这里 rc 停在
     * -1，整轮产物被当作失败丢弃，调用方只得到一句通用错误。 */
    if (more) {
        rc = AIRY_EBUDGET;
        if (!r.text)
            r.text = AIRY_STRDUP("Tool round budget exhausted; resume with the returned token");
        /* 暂停即可续跑：令牌自包含已执行步骤与待续目标，客户端不必重发历史。 */
        out->resume_token = run_resume_emit(&r);
    }
    run_loop_ckpt(&r, out, cancelled, rc);
    return run_loop_fin(&r, out, rc);
}
