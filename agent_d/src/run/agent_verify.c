/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_verify.c
 * @brief C-17 完成判据外部化：把 success_criteria 交给 tool_d verifier 只读
 *        复核，并把结果归因为三种终局（通过 / 未过 / 无法取证）。
 *
 * 职责边界：本文件只搬运、重试、归因；判据语法与取证方式全部在 tool_d
 * （verify/tool_verify.c）。agent_d 不解析判据内容，因此新增判据类型无需
 * 改动本文件。
 */

#include "agent_verify.h"

#include "agent_run_engine.h"

#include "airy_memory.h"
#include "airy_verify.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "platform_misc.h"
#include "platform_paths.h"
#include "svc_logger.h"

#include <string.h>

/* tool_d 端点名（AIRY_VERIFY_DAEMON="tool"）：socket 名由 daemon 目录约定
 * 固定为 <daemon>.sock。路径必须经 airy_runtime_dir_socket 解析到运行目录：
 * daemon_rpc_call 直接使用传入路径（不补目录），传裸名只在 cwd 恰为运行目录
 * 时才能连通，会令 verifier 恒不可达。 */
#define AV_SOCK "tool.sock"
/* RPC 时限必须大于 verifier 自身预算，否则复核尚未完成就被调用方掐断。 */
#define AV_RPC_MS (AIRY_VERIFY_DEFAULT_TIMEOUT_MS + 10000)

/* 判据既可写数组本身，也可写 {"criteria":[...],"on_fail":...} 包装对象；
 * 两种线形态在此归一为数组副本（后续只传数组给 verifier）。 */
static cJSON *av_criteria_dup(const char *criteria)
{
    if (!criteria || !criteria[0])
        return NULL;
    cJSON *root = cJSON_Parse(criteria);
    if (!root)
        return NULL;
    const cJSON *arr = cJSON_IsArray(root)
                           ? root
                           : cJSON_GetObjectItemCaseSensitive(root, AIRY_VK_CRITERIA);
    cJSON *dup = NULL;
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0)
        dup = cJSON_Duplicate(arr, 1);
    cJSON_Delete(root);
    return dup;
}

/* on_fail=continue：判据未过不阻断本次 run（处置策略归调用方声明，机制层
 * 只执行）。缺省与其余取值一律按 fail 收尾，保持 fail-closed。 */
static int av_continue_allowed(const char *criteria)
{
    cJSON *root = cJSON_Parse(criteria ? criteria : "");
    if (!root)
        return 0;
    const cJSON *of = cJSON_GetObjectItemCaseSensitive(root, AIRY_VK_ON_FAIL);
    int cont = cJSON_IsString(of) && strcmp(of->valuestring, AIRY_VF_CONTINUE) == 0;
    cJSON_Delete(root);
    return cont;
}

/* 单次调用 tool.verify；传输失败（不可达/超时/坏载荷）返回 NULL。 */
static cJSON *av_call_once(const cJSON *criteria)
{
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return NULL;
    cJSON_AddItemToObject(params, AIRY_VK_CRITERIA, cJSON_Duplicate(criteria, 1));
    cJSON_AddNumberToObject(params, AIRY_VK_TIMEOUT_MS, AIRY_VERIFY_DEFAULT_TIMEOUT_MS);
    char *req = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!req)
        return NULL;

    char *resp = NULL;
    int rc = daemon_rpc_call(airy_runtime_dir_socket(AV_SOCK), AIRY_VERIFY_METHOD, req, &resp,
                             AV_RPC_MS);
    AIRY_FREE(req);
    if (rc != AIRY_SUCCESS || !resp) {
        AIRY_FREE(resp);
        return NULL;
    }
    cJSON *out = cJSON_Parse(resp);
    AIRY_FREE(resp);
    return out;
}

int agent_verify_apply(const char *criteria, cJSON **out_verification)
{
    if (out_verification)
        *out_verification = NULL;

    cJSON *arr = av_criteria_dup(criteria);
    if (!arr) {
        SVC_LOG_WARN("agent.verify: criteria missing or unparsable, cannot verify");
        return AGENT_RUN_RC_UNVERIFIED;
    }

    cJSON *res = NULL;
    for (int i = 0; i < AGENT_VERIFY_RETRY && !res; ++i) {
        if (i > 0)
            airy_sleep_ms(AGENT_VERIFY_RETRY_MS);
        res = av_call_once(arr);
    }
    cJSON_Delete(arr);

    if (!res) {
        SVC_LOG_WARN("agent.verify: tool.verify unreachable after %d attempts",
                     AGENT_VERIFY_RETRY);
        return AGENT_RUN_RC_UNVERIFIED;
    }

    int ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, AIRY_VK_OK));
    if (out_verification)
        *out_verification = res;
    else
        cJSON_Delete(res);

    if (ok)
        return 0;
    return av_continue_allowed(criteria) ? 0 : AGENT_RUN_RC_UNMET;
}
