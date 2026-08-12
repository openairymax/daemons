// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_client.c
 * @brief Service communication client (client domain).
 *
 * Implements the service-client interface defined in svc_common.h: creates
 * and destroys clients per protocol. Supports SVC_PROTO_HTTP (libcurl
 * remote call or local service-handle direct connect) and SVC_PROTO_MEMORY
 * (local service-handle direct connect, falling back to IPC RPC on miss).
 *
 * The local direct-connect path accesses the service instance's
 * iface/user_data fields directly via airy_svc_internal_t exposed by
 * svc_common_internal.h (sharing that struct definition with the service
 * lifecycle domain); everything else relies only on svc_common.h public
 * APIs.
 *
 * @see agentrt/daemons/common/include/svc_common.h
 * @see agentrt/daemons/common/src/svc_common.c
 * @see agentrt/daemons/common/src/svc_common_internal.h
 */

#include "svc_common.h"
#include "svc_common_internal.h"

#include "airy_memory.h"
#include "daemon_errors.h"
#include "ipc_client.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CURL
#include <curl/curl.h>
#endif

typedef struct {
    airy_svc_protocol_type_t protocol;
    char base_url[512];
    uint32_t default_timeout_ms;
} client_internal_t;

#ifdef AIRY_HAS_CURL
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} curl_response_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_response_buf_t *buf = (curl_response_buf_t *)userdata;
    size_t total = size * nmemb;
    if (buf->size + total + 1 > buf->capacity) {
        size_t new_cap = buf->capacity == 0 ? 4096 : buf->capacity;
        while (new_cap < buf->size + total + 1)
            new_cap *= 2;
        char *new_data = (char *)AIRY_REALLOC(buf->data, new_cap);
        if (!new_data)
            return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    if (buf->size + total > buf->capacity) {
        airy_err_push_ex(AIRY_EOVERFLOW, __FILE__, __LINE__, __func__,
                         "curl_write_cb: buffer overflow");
        return 0;
    }
    __builtin_memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}
#endif

static airy_err_t http_client_call(const char *service_name, const char *method,
                                   const char *params_json, char **response_json,
                                   uint32_t timeout_ms)
{
    if (!service_name || !method || !response_json) {
        return AIRY_EINVAL;
    }

    LOG_DEBUG("HTTP client call: %s/%s (timeout=%ums)", service_name, method, timeout_ms);

    *response_json = NULL;

    airy_svc_t svc = airy_svc_find(service_name);
    if (svc) {
        airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

        if (internal->iface.handle_request) {
            airy_err_t err = internal->iface.handle_request(svc, method, params_json, response_json,
                                                            internal->user_data);
            if (err != AIRY_SUCCESS) {
                LOG_WARN("Service '%s' handle_request('%s') failed: %d", service_name, method, err);
                return err;
            }
            if (!*response_json) {
                *response_json = AIRY_CALLOC(1, 2);
                if (*response_json) {
                    (*response_json)[0] = '{';
                    (*response_json)[1] = '}';
                }
            }
            return AIRY_SUCCESS;
        }

        airy_svc_state_t state = airy_svc_get_state(svc);
        if (state != AIRY_SVC_STATE_RUNNING) {
            LOG_WARN("Service '%s' not running (state=%s), cannot handle request", service_name,
                     airy_svc_state_to_string(state));
            return DAEMON_ESTATE;
        }

        LOG_WARN("Service '%s' has no handle_request callback", service_name);
        return AIRY_ESERVICE;
    }

    char url[768];
    snprintf(url, sizeof(url), "http://%s/api/%s", service_name, method);

#ifdef AIRY_HAS_CURL
    airy_err_t ret_err = AIRY_SUCCESS;
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize CURL for remote call to '%s'", service_name);
        return DAEMON_EINIT;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);

    if (params_json) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params_json);
    }

    curl_response_buf_t resp_buf = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR("CURL call to '%s' failed: %s", url, curl_easy_strerror(res));
        ret_err = AIRY_EIO;
        goto cleanup;
    }

    if (http_code >= 400) {
        LOG_ERROR("Service '%s' returned HTTP %ld", service_name, http_code);
        ret_err = AIRY_EIO;
        goto cleanup;
    }

    if (!resp_buf.data || resp_buf.size == 0) {
        *response_json = AIRY_CALLOC(1, 2);
        if (*response_json) {
            (*response_json)[0] = '{';
            (*response_json)[1] = '}';
        }
    } else {
        *response_json = resp_buf.data;
    }

    return AIRY_SUCCESS;

cleanup:
    AIRY_FREE(resp_buf.data);
    return ret_err;
#else
    LOG_ERROR("Remote call to '%s' failed: libcurl not available", service_name);
    return AIRY_EIO;
#endif
}

static airy_err_t http_client_stream(const char *service_name, const char *method,
                                     const char *params_json, airy_stream_callback_t callback,
                                     void *user_data)
{
    if (!service_name || !method || !callback) {
        return AIRY_EINVAL;
    }

    LOG_DEBUG("HTTP client stream: %s/%s", service_name, method);

    airy_svc_t svc = airy_svc_find(service_name);
    if (svc) {
        airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

        if (internal->iface.handle_request) {
            char *response = NULL;
            airy_err_t err = internal->iface.handle_request(svc, method, params_json, &response,
                                                            internal->user_data);
            if (err == AIRY_SUCCESS && response) {
                callback(response, strlen(response), user_data);
                AIRY_FREE(response);
            } else {
                const char *err_json = "{\"error\":\"stream_failed\"}";
                callback(err_json, strlen(err_json), user_data);
            }
            return err;
        }

        const char *err_json = "{\"error\":\"no_stream_handler\"}";
        callback(err_json, strlen(err_json), user_data);
        return AIRY_ESERVICE;
    }

    return AIRY_ENOENT;
}

static airy_err_t memory_client_call(const char *service_name, const char *method,
                                     const char *params_json, char **response_json,
                                     uint32_t timeout_ms)
{
    if (!service_name || !method || !response_json) {
        return AIRY_EINVAL;
    }

    airy_svc_t svc = airy_svc_find(service_name);
    if (svc) {
        airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

        if (internal->iface.handle_request) {
            airy_err_t err = internal->iface.handle_request(svc, method, params_json, response_json,
                                                            internal->user_data);
            if (err != AIRY_SUCCESS) {
                LOG_WARN("Service '%s' handle_request('%s') failed: %d", service_name, method, err);
                return err;
            }
            if (!*response_json) {
                *response_json = (char *)AIRY_CALLOC(1, 2);
                if (*response_json) {
                    (*response_json)[0] = '{';
                    (*response_json)[1] = '}';
                }
            }
            return AIRY_SUCCESS;
        }

        LOG_WARN("Service '%s' has no handle_request callback", service_name);
        return AIRY_ESERVICE;
    }

    LOG_INFO("Memory client: service '%s' not found locally, trying IPC RPC", service_name);

    char rpc_method[256];
    snprintf(rpc_method, sizeof(rpc_method), "%s.%s", service_name, method);

    int rpc_err =
        svc_rpc_call(rpc_method, params_json, response_json, timeout_ms ? timeout_ms : 30000);
    if (rpc_err != 0 || !(*response_json)) {
        LOG_WARN("IPC RPC call to '%s' failed: %d", rpc_method, rpc_err);
        return AIRY_EIO;
    }

    LOG_INFO("IPC RPC call to '%s' succeeded", rpc_method);
    return AIRY_SUCCESS;
}

static airy_err_t memory_client_stream(const char *service_name, const char *method,
                                       const char *params_json, airy_stream_callback_t callback,
                                       void *user_data)
{
    if (!service_name || !method || !callback) {
        return AIRY_EINVAL;
    }

    airy_svc_t svc = airy_svc_find(service_name);
    if (!svc) {
        return AIRY_ENOENT;
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

    if (internal->iface.handle_request) {
        char *response = NULL;
        airy_err_t err = internal->iface.handle_request(svc, method, params_json, &response,
                                                        internal->user_data);
        if (err == AIRY_SUCCESS && response) {
            callback(response, strlen(response), user_data);
            AIRY_FREE(response);
        } else {
            const char *err_json = "{\"error\":\"stream_failed\"}";
            callback(err_json, strlen(err_json), user_data);
        }
        return err;
    }

    const char *err_json = "{\"error\":\"no_stream_handler\"}";
    callback(err_json, strlen(err_json), user_data);
    return AIRY_ESERVICE;
}

airy_err_t airy_svc_client_create(airy_svc_protocol_type_t protocol, const char *config,
                                  airy_svc_client_t **client)
{
    if (!client) {
        return AIRY_EINVAL;
    }

    client_internal_t *internal = (client_internal_t *)AIRY_CALLOC(1, sizeof(client_internal_t));
    if (!internal) {
        return AIRY_ENOMEM;
    }

    internal->protocol = protocol;
    internal->default_timeout_ms = 30000;

    if (config) {
        if (safe_strcpy(internal->base_url, config, sizeof(internal->base_url)) != 0) {
            AIRY_FREE(internal);
            return AIRY_EINVAL;
        }
    } else {
        if (safe_strcpy(internal->base_url, "http://localhost:8080", sizeof(internal->base_url)) !=
            0) {
            AIRY_FREE(internal);
            return AIRY_EINVAL;
        }
    }

    airy_svc_client_t *cli = (airy_svc_client_t *)AIRY_CALLOC(1, sizeof(airy_svc_client_t));
    if (!cli) {
        AIRY_FREE(internal);
        return AIRY_ENOMEM;
    }

    switch (protocol) {
    case SVC_PROTO_HTTP:
        cli->call = http_client_call;
        cli->stream = http_client_stream;
        break;
    case SVC_PROTO_MEMORY:
        cli->call = memory_client_call;
        cli->stream = memory_client_stream;
        break;
    default:
        cli->call = http_client_call;
        cli->stream = http_client_stream;
        LOG_WARN("Protocol %d not fully implemented, using HTTP fallback", protocol);
        break;
    }

    *client = cli;
    cli->internal = internal;

    LOG_INFO("Service client created (protocol=%d, base_url=%s)", protocol, internal->base_url);
    return AIRY_SUCCESS;
}

void airy_svc_client_destroy(airy_svc_client_t *client)
{
    if (!client) {
        return;
    }
    if (client->internal) {
        AIRY_FREE(client->internal);
        client->internal = NULL;
    }
    AIRY_FREE(client);
    LOG_DEBUG("Service client destroyed");
}
