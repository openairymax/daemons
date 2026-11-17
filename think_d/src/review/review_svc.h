// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file review_svc.h
 * @brief think.review 执行复核服务面声明（M1-1c，实现见 review_svc.c）。
 */

#ifndef THINK_D_REVIEW_SVC_H
#define THINK_D_REVIEW_SVC_H

#include "airy_memory.h"
#include "llm_svc_adapter.h"

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 生命周期：daemon 启动注入 llm 适配器与 t2/t1-f 模型；收尾释放引用 */
int review_svc_init(llm_svc_adapter_t *adapter, const char *t2_model,
                    const char *t1f_model);
void review_svc_cleanup(void);

/* think.review：执行复核判断（t2 语义审查 / t1-f 终裁）。
 * 签名对齐 method_fn：第三参数为 dispatcher 传入的 client_fd 指针。 */
void review_svc_process(cJSON *params, int id, void *user_data);

/* C-4：复核输出 schema 契约（纯函数，服务面与黄金集共用）。
 *
 * 严格解析模型输出中的单行 JSON：必须同时满足
 *   - 存在 `key` 且为数值 0/1（字符串 "1"、越界值一律不合法）
 *   - 存在 `reason` 且为非空字符串（≤200 字节）
 *   - JSON 对象两端允许被思考前缀/后缀包裹，但截取后必须完整可解析
 * 返回 1=合法（raw 写 0/1，reason 有空间则写入），0=不合法。
 * 缺省策略 fail：不合法即判「无法判断」，由机制层降级；置
 * AIRY_REVIEW_SCHEMA=warn 时才回落到旧式子串提取（仅兼容用途）。 */
int rv_schema_parse(const char *content, const char *key, int *raw,
                    char *reason, size_t sz);

/* 0=fail（缺省）/ 1=warn，取自 AIRY_REVIEW_SCHEMA。 */
int rv_schema_mode(void);

/* t1-f 的 accept→verdict 映射：accept=0 拒绝→verdict=1；accept=1→0。 */
static inline int rv_accept_map(int raw)
{
    return raw == 0 ? 1 : 0;
}

/* 提示词装配（C-4：B 侧读正文）。body 为产物正文，优先阅读；
 * json 只作结构字段对照。返回 0 成功，-1 参数错。 */
int rv_prompt_t2(char *buf, size_t sz, const char *goal, const char *sigs,
                 const char *body, const char *json);
int rv_prompt_t1f(char *buf, size_t sz, const char *gate, int drift,
                  const char *goal, const char *body, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* THINK_D_REVIEW_SVC_H */
