// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "daemon_errors.h"
#include "jsonrpc_helpers.h"
#include "airy_memory.h"
#include "method_dispatcher.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include "error.h"

struct method_handler {
    char *method;
    method_fn handler;
    void *user_data;
};

struct method_dispatcher {
    struct method_handler *handlers;
    size_t max_methods;
    size_t method_count;
    airy_mtx_t lock;
};

static int find_method_index(method_dispatcher_t *disp, const char *method)
{
    for (size_t i = 0; i < disp->method_count; i++) {
        if (strcmp(disp->handlers[i].method, method) == 0)
            return (int)i;
    }
    return AIRY_ERR_NOT_FOUND;
}

method_dispatcher_t *method_dispatcher_create(size_t max_methods)
{
    if (max_methods == 0) {
        SVC_LOG_ERROR("method_dispatcher_create: max_methods is zero");
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    method_dispatcher_t *disp =
        (method_dispatcher_t *)AIRY_CALLOC(1, sizeof(method_dispatcher_t));
    if (!disp) {
        SVC_LOG_ERROR("method_dispatcher_create: memory allocation failed for dispatcher");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    disp->handlers =
        (struct method_handler *)AIRY_CALLOC(max_methods, sizeof(struct method_handler));
    if (!disp->handlers) {
        SVC_LOG_ERROR("method_dispatcher_create: memory allocation failed for handlers (max_methods=%zu)", max_methods);
        AIRY_FREE(disp);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    disp->max_methods = max_methods;
    disp->method_count = 0;
    airy_mtx_init(&disp->lock);

    return disp;
}

void method_dispatcher_destroy(method_dispatcher_t *disp)
{
    if (!disp)
        return;

    for (size_t i = 0; i < disp->method_count; i++) {
        AIRY_FREE(disp->handlers[i].method);
    }
    AIRY_FREE(disp->handlers);
    airy_mtx_destroy(&disp->lock);
    AIRY_FREE(disp);
}

int method_dispatcher_register(method_dispatcher_t *disp, const char *method, method_fn handler,
                               void *user_data)
{
    if (!disp || !method || !handler) {
        SVC_LOG_ERROR("method_dispatcher_register: null parameter disp=%p method=%p handler=%p",
                      (void *)disp, (void *)method, (void *)(uintptr_t)handler);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (disp->method_count >= disp->max_methods) {
        SVC_LOG_ERROR("method_dispatcher_register: max methods reached count=%zu max=%zu method='%s'",
                      disp->method_count, disp->max_methods, method);
        return AIRY_ERR_OVERFLOW;
    }

    int existing = find_method_index(disp, method);
    if (existing >= 0) {
        SVC_LOG_WARN("method_dispatcher_register: method '%s' already registered at index=%d", method, existing);
        return AIRY_ERR_UNKNOWN;
    }

    disp->handlers[disp->method_count].method = AIRY_STRDUP(method);
    disp->handlers[disp->method_count].handler = handler;
    disp->handlers[disp->method_count].user_data = user_data;
    disp->method_count++;

    return 0;
}

int method_dispatcher_dispatch(method_dispatcher_t *disp, cJSON *request,
                               char *(*error_response_fn)(int, const char *, int), void *user_data)
{
    if (!disp || !request) {
        SVC_LOG_ERROR("method_dispatcher_dispatch: null parameter disp=%p request=%p",
                      (void *)disp, (void *)request);
        return AIRY_ERR_INVALID_PARAM;
    }

    char *method = NULL;
    cJSON *params = NULL;
    int id = 0;

    if (jsonrpc_parse_request_ptr(request, &method, &params, &id) != 0) {
        SVC_LOG_ERROR("method_dispatcher_dispatch: failed to parse JSON-RPC request");
        if (error_response_fn) {
            char *err = error_response_fn(JSONRPC_INVALID_REQUEST, "Invalid request", id);
            AIRY_FREE(err);
        }
        return AIRY_ERR_PARSE_ERROR;
    }

    int index = find_method_index(disp, method);
    if (index < 0) {
        /* 命名空间兼容：外部客户端按 "namespace.method"（如 agent.spawn）调用，
           而各 daemon 注册使用短名（spawn）。剥离命名空间前缀后二次匹配，
           避免按文档全名调用时 method not found。 */
        const char *dot = strchr(method, '.');
        if (dot && dot[1] != '\0') {
            index = find_method_index(disp, dot + 1);
            if (index >= 0) {
                SVC_LOG_DEBUG("method_dispatcher_dispatch: matched '%s' via short name '%s'",
                              method, dot + 1);
            }
        }
    }
    if (index < 0) {
        SVC_LOG_WARN("method_dispatcher_dispatch: method '%s' not found (registered=%zu)", method, disp->method_count);
        if (error_response_fn) {
            char *err = error_response_fn(JSONRPC_METHOD_NOT_FOUND, "Method not found", id);
            AIRY_FREE(err);
        }
        AIRY_FREE(method);
        if (params)
            cJSON_Delete(params);
        return AIRY_ERR_NOT_FOUND;
    }

    method_fn handler = disp->handlers[index].handler;
    void *data = disp->handlers[index].user_data ? disp->handlers[index].user_data : user_data;

    handler(params, id, data);

    AIRY_FREE(method);
    if (params)
        cJSON_Delete(params);

    return 0;
}
