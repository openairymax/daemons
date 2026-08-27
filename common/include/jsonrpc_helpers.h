/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file jsonrpc_helpers.h
 * @brief JSON-RPC 2.0 common helper library.
 */

#ifndef AIRY_RT_JSONRPC_HELPERS_H
#define AIRY_RT_JSONRPC_HELPERS_H

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#include "airy_memory.h"
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


/**
 * @brief Send a JSON-RPC error response to the client (build+send+free).
 * @param socket Client socket descriptor
 * @param error_code Error code
 * @param message Error message
 * @param id Request ID
 * @note Replaces the manual build_error -> send -> free three-liner
 */
#define JSONRPC_SEND_ERROR(socket, error_code, message, id)              \
    do {                                                                 \
        char *_err = jsonrpc_build_error((error_code), (message), (id)); \
        if (_err) {                                                      \
            airy_sock_send((socket), _err, strlen(_err));                \
            AIRY_FREE(_err);                                             \
        }                                                                \
    } while (0)

/**
 * @brief Send a JSON-RPC success response to the client (build+send+free).
 * @param socket Client socket descriptor
 * @param result cJSON result object (ownership transfers to the root
 *               inside jsonrpc_build_success, recursively freed by
 *               cJSON_Delete(root); caller must not free it again)
 * @param id Request ID
 * @note Replaces the manual build_success -> send -> free three-liner
 * @warning jsonrpc_build_success attaches result to its root via
 *          cJSON_AddItemToObject, then cJSON_Delete(root) recursively frees
 *          result. This macro must not call cJSON_Delete(result) again or it
 *          double-frees. If result carries CJSON_AUTO_FREE, set result=NULL
 *          after the macro.
 */
#define JSONRPC_SEND_SUCCESS(socket, result, id)                                            \
    do {                                                                                    \
        char *_success = jsonrpc_build_success((result), (id));                             \
        /* result ownership already moved into jsonrpc_build_success's root; do not Delete */\
        if (_success) {                                                                     \
            airy_sock_send((socket), _success, strlen(_success));                           \
            AIRY_FREE(_success);                                                            \
        }                                                                                   \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_JSONRPC_HELPERS_H */
