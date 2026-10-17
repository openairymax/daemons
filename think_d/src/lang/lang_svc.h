// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file lang_svc.h
 * @brief think.* 推理语言网关服务面声明（M1-1c，实现见 lang_svc.c）。
 */

#ifndef THINK_D_LANG_SVC_H
#define THINK_D_LANG_SVC_H

#include "airy_memory.h"

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 生命周期：daemon 启动/收尾各调用一次 */
int lang_svc_init(void);
void lang_svc_cleanup(void);

/* think.lang_process / think.lang_postprocess / think.lang_stats。
 * 签名对齐 method_fn（method_dispatcher.h）：第三参数为 dispatcher 传入
 * 的 client_fd 指针（void *），内部解引用为 airy_sock_t。 */
void lang_svc_process(cJSON *params, int id, void *user_data);
void lang_svc_postprocess(cJSON *params, int id, void *user_data);
void lang_svc_stats(cJSON *params, int id, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* THINK_D_LANG_SVC_H */
