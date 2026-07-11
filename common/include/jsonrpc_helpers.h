// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file jsonrpc_helpers.h
 * @brief JSON-RPC 2.0 公共辅助函数库
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 * @version 0.1.0
 * @date 2026-04-04
 */

#ifndef AIRY_RT_JSONRPC_HELPERS_H
#define AIRY_RT_JSONRPC_HELPERS_H

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"

#ifndef AIRY_API
#if defined(_WIN32) && defined(AIRY_BUILD_DLL)
#define AIRY_API __declspec(dllexport)
#elif defined(_WIN32)
#define AIRY_API __declspec(dllimport)
#else
#define AIRY_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define JSONRPC_PARSE_ERROR (-32700)
#define JSONRPC_INVALID_REQUEST (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS (-32602)
#define JSONRPC_INTERNAL_ERROR (-32603)

/* 自定义错误码范围：-32000 到 -32099（JSON-RPC 2.0 服务器错误区间） */
#define JSONRPC_SERVER_ERROR_BASE (-32000)
#define JSONRPC_RATE_LIMITED (-32001)
#define JSONRPC_AUTH_FAILED (-32002)
#define JSONRPC_SESSION_EXPIRED (-32003)
#define JSONRPC_SERVICE_UNAVAILABLE (-32004)

AIRY_API char *jsonrpc_build_error(int code, const char *message, int id);
AIRY_API char *jsonrpc_build_success(cJSON *result, int id);
AIRY_API char *jsonrpc_build_success_string(const char *result_str, int id);
AIRY_API int jsonrpc_parse_request(const char *raw, char **out_method, cJSON **out_params,
                                      int *out_id);
AIRY_API int jsonrpc_parse_request_ptr(cJSON *req, char **out_method, cJSON **out_params,
                                          int *out_id);
AIRY_API int jsonrpc_validate_request(cJSON *req);
AIRY_API const char *jsonrpc_get_string_param(cJSON *params, const char *key,
                                                 const char *default_value);
AIRY_API int jsonrpc_get_int_param(cJSON *params, const char *key, int default_value);
AIRY_API int jsonrpc_get_bool_param(cJSON *params, const char *key, int default_value);
AIRY_API cJSON *jsonrpc_get_array_param(cJSON *params, const char *key);
AIRY_API cJSON *jsonrpc_get_object_param(cJSON *params, const char *key);
AIRY_API int jsonrpc_is_notification(cJSON *req);
AIRY_API char *jsonrpc_build_notification(const char *method, cJSON *params);
AIRY_API const char *jsonrpc_get_error_message(int code);
AIRY_API char *jsonrpc_build_error_with_data(int code, const char *message, cJSON *data, int id);
AIRY_API int jsonrpc_is_batch_request(const char *raw);

/* ==================== 响应发送辅助宏（消除重复代码） ==================== */

/**
 * @brief 发送 JSON-RPC 错误响应到客户端（自动构建+发送+释放）
 * @param socket 客户端 socket 描述符
 * @param error_code 错误码
 * @param message 错误消息
 * @param id 请求 ID
 * @note 替代手动: build_error → send → free 三行组合
 */
#define JSONRPC_SEND_ERROR(socket, error_code, message, id)              \
    do {                                                                 \
        char *_err = jsonrpc_build_error((error_code), (message), (id)); \
        if (_err) {                                                      \
            airy_sock_send((socket), _err, strlen(_err));           \
            AIRY_FREE(_err);                                          \
        }                                                                \
    } while (0)

/**
 * @brief 发送 JSON-RPC 成功响应到客户端（自动构建+发送+释放）
 * @param socket 客户端 socket 描述符
 * @param result cJSON 结果对象（所有权转移给 jsonrpc_build_success 内部 root，
 *                由 cJSON_Delete(root) 递归释放，调用方不可再次释放）
 * @param id 请求 ID
 * @note 替代手动: build_success → send → free 三行组合
 * @warning jsonrpc_build_success 通过 cJSON_AddItemToObject 将 result 挂到 root 上，
 *          随后 cJSON_Delete(root) 递归释放了 result。此宏不可再调用 cJSON_Delete(result)，
 *          否则 double-free。若 result 变量带 CJSON_AUTO_FREE，调用方需在宏后置 result=NULL。
 */
#define JSONRPC_SEND_SUCCESS(socket, result, id)                       \
    do {                                                               \
        char *_success = jsonrpc_build_success((result), (id));        \
        /* result 所有权已转移至 jsonrpc_build_success 内部 root 并被释放，不可再 Delete */ \
        if (_success) {                                                \
            airy_sock_send((socket), _success, strlen(_success)); \
            AIRY_FREE(_success);                                    \
        }                                                              \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_JSONRPC_HELPERS_H */
