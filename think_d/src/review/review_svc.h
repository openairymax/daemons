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

#ifdef __cplusplus
}
#endif

#endif /* THINK_D_REVIEW_SVC_H */
