// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file ipc_client.c
 * @brief IPC 客户端实现（线程安全版本）
 *
 * 改进：
 * 1. 线程安全的连接池
 * 2. 修复内存安全问题
 * 3. 支持连接复用
 * 4. 添加超时和重试机制
 */

#include "daemon_platform_ext.h"
#include "svc_common.h"
#include "svc_config.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef airy_err_push_ex
void airy_err_push_ex(int code, const char *file, int line, const char *func, const char *fmt, ...);
#endif

#define IPC_POOL_SIZE 4
#define IPC_DEFAULT_TIMEOUT_MS 30000
#define IPC_MAX_RESPONSE_SIZE (16 * 1024 * 1024) /* 16MB */
#define IPC_MAX_RETRIES 3
#define IPC_RETRY_DELAY_MS 100

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} ipc_response_buffer_t;

typedef struct {
    CURL *curl;
    airy_mtx_t lock;
    int in_use;
    uint64_t last_used;
} ipc_pool_entry_t;

struct ipc_client {
    char *base_url;
    ipc_pool_entry_t pool[IPC_POOL_SIZE];
    airy_mtx_t pool_lock;
    uint32_t default_timeout_ms;
    int initialized;
};

static struct ipc_client *g_ipc_client = NULL;
static airy_mtx_t g_init_lock = {0};
static int g_curl_initialized = 0;

/**
 * @brief Initialize the response buffer
 */
static int buffer_init(ipc_response_buffer_t *buf)
{
    buf->capacity = 4096;
    buf->data = (char *)AIRY_MALLOC(buf->capacity);
    if (!buf->data) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    buf->data[0] = '\0';
    buf->size = 0;
    return 0;
}

/**
 * @brief Free the response buffer
 */
static void buffer_free(ipc_response_buffer_t *buf)
{
    if (buf->data) {
        AIRY_FREE(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

/**
 * @brief libcurl write callback (memory-safe version)
 */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    ipc_response_buffer_t *buf = (ipc_response_buffer_t *)userp;

    if (buf->size + realsize > IPC_MAX_RESPONSE_SIZE) {
        return 0;
    }

    size_t new_size = buf->size + realsize + 1;
    if (new_size > buf->capacity) {
        size_t new_capacity = buf->capacity * 2;
        if (new_capacity < new_size) {
            new_capacity = new_size;
        }

        char *new_data = (char *)AIRY_REALLOC(buf->data, new_capacity);
        if (!new_data) {
            return 0;
        }

        buf->data = new_data;
        buf->capacity = new_capacity;
    }

    __builtin_memcpy(buf->data + buf->size, contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = '\0';

    return realsize;
}

/**
 * @brief Acquire an available connection from the pool
 */
static ipc_pool_entry_t *pool_acquire(struct ipc_client *client)
{
    ipc_pool_entry_t *entry = NULL;

    airy_mtx_lock(&client->pool_lock);

    for (int i = 0; i < IPC_POOL_SIZE; i++) {
        if (!client->pool[i].in_use) {
            entry = &client->pool[i];
            entry->in_use = 1;
            break;
        }
    }

    airy_mtx_unlock(&client->pool_lock);

    if (!entry) {
        for (int retry = 0; retry < 10; retry++) {
            airy_mtx_lock(&client->pool_lock);
            for (int i = 0; i < IPC_POOL_SIZE; i++) {
                if (!client->pool[i].in_use) {
                    entry = &client->pool[i];
                    entry->in_use = 1;
                    break;
                }
            }
            airy_mtx_unlock(&client->pool_lock);

            if (entry)
                break;

            airy_mtx_lock(&client->pool_lock);
            airy_mtx_unlock(&client->pool_lock);
        }
    }

    return entry;
}

/**
 * @brief Release a connection back to the pool
 */
static void pool_release(struct ipc_client *client, ipc_pool_entry_t *entry)
{
    if (entry) {
        airy_mtx_lock(&client->pool_lock);
        entry->in_use = 0;
        entry->last_used = airy_time_ms();
        airy_mtx_unlock(&client->pool_lock);
    }
}

/**
 * @brief Execute an RPC call (with retries)
 */
static int do_rpc_call(ipc_pool_entry_t *entry, const char *base_url, const char *request,
                       ipc_response_buffer_t *response, uint32_t timeout_ms, int max_retries)
{
    CURLcode res;
    long http_code = 0;
    int retry = 0;

    while (retry <= max_retries) {

        curl_easy_reset(entry->curl);

        curl_easy_setopt(entry->curl, CURLOPT_URL, base_url);
        curl_easy_setopt(entry->curl, CURLOPT_POST, 1L);
        curl_easy_setopt(entry->curl, CURLOPT_POSTFIELDS, request);
        curl_easy_setopt(entry->curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(entry->curl, CURLOPT_WRITEDATA, response);
        curl_easy_setopt(entry->curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
        curl_easy_setopt(entry->curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(entry->curl, CURLOPT_FOLLOWLOCATION, 1L);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(entry->curl, CURLOPT_HTTPHEADER, headers);

        res = curl_easy_perform(entry->curl);
        curl_slist_free_all(headers);

        if (res == CURLE_OK) {
            curl_easy_getinfo(entry->curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code == 200) {
                return SVC_OK;
            }
        }

        response->size = 0;
        if (response->data) {
            response->data[0] = '\0';
        }

        retry++;

        if (retry <= max_retries) {
            uint32_t delay = IPC_RETRY_DELAY_MS * retry;
            airy_sleep_ms(delay);
        }
    }

    return SVC_ERR_RPC;
}

#define SVC_ERROR(code, msg)                                                 \
    do {                                                                     \
        airy_err_push_ex((code), __FILE__, __LINE__, __func__, "%s", (msg)); \
        return (code);                                                       \
    } while (0)

int svc_ipc_init(const char *baseruntime_url)
{
    struct ipc_client *client = NULL;

    if (!baseruntime_url) {
        SVC_ERROR(SVC_ERR_INVALID_PARAM, "baseruntime_url is NULL");
    }

    airy_mtx_lock(&g_init_lock);

    if (!g_curl_initialized) {
        CURLcode curl_res = curl_global_init(CURL_GLOBAL_ALL);
        if (curl_res != CURLE_OK) {
            airy_mtx_unlock(&g_init_lock);
            SVC_ERROR(SVC_ERR_IO, "Failed to initialize libcurl");
        }
        g_curl_initialized = 1;
    }

    if (g_ipc_client) {
        airy_mtx_unlock(&g_init_lock);
        return SVC_OK;
    }

    client = (struct ipc_client *)AIRY_CALLOC(1, sizeof(struct ipc_client));
    if (!client) {
        airy_mtx_unlock(&g_init_lock);
        SVC_ERROR(SVC_ERR_OUT_OF_MEMORY, "Failed to allocate IPC client");
    }

    client->base_url = AIRY_STRDUP(baseruntime_url);
    if (!client->base_url) {
        AIRY_FREE(client);
        airy_mtx_unlock(&g_init_lock);
        SVC_ERROR(SVC_ERR_OUT_OF_MEMORY, "Failed to duplicate base URL");
    }

    client->default_timeout_ms = IPC_DEFAULT_TIMEOUT_MS;
    if (airy_mtx_init(&client->pool_lock) != 0) {
        AIRY_FREE(client->base_url);
        AIRY_FREE(client);
        airy_mtx_unlock(&g_init_lock);
        SVC_ERROR(SVC_ERR_OUT_OF_MEMORY, "Failed to initialize pool mutex");
    }

    int i;
    for (i = 0; i < IPC_POOL_SIZE; i++) {
        client->pool[i].curl = curl_easy_init();
        if (!client->pool[i].curl) {

            airy_err_push_ex(SVC_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "Failed to initialize CURL handle %d", i);
            break;
        }
        if (airy_mtx_init(&client->pool[i].lock) != 0) {
            curl_easy_cleanup(client->pool[i].curl);
            client->pool[i].curl = NULL;
            airy_err_push_ex(SVC_ERR_OUT_OF_MEMORY, __FILE__, __LINE__, __func__,
                             "Failed to initialize pool entry mutex %d", i);
            break;
        }
        client->pool[i].in_use = 0;
        client->pool[i].last_used = 0;
    }

    if (i < IPC_POOL_SIZE) {

        for (int j = 0; j < i; j++) {
            if (client->pool[j].curl) {
                curl_easy_cleanup(client->pool[j].curl);
            }
            airy_mtx_destroy(&client->pool[j].lock);
        }
        airy_mtx_destroy(&client->pool_lock);
        AIRY_FREE(client->base_url);
        AIRY_FREE(client);
        airy_mtx_unlock(&g_init_lock);
        return SVC_ERR_OUT_OF_MEMORY;
    }

    client->initialized = 1;
    g_ipc_client = client;

    airy_mtx_unlock(&g_init_lock);
    return SVC_OK;
}

#undef SVC_ERROR

void svc_ipc_cleanup(void)
{
    airy_mtx_lock(&g_init_lock);

    if (g_ipc_client) {

        for (int i = 0; i < IPC_POOL_SIZE; i++) {
            if (g_ipc_client->pool[i].curl) {
                curl_easy_cleanup(g_ipc_client->pool[i].curl);
            }
            airy_mtx_destroy(&g_ipc_client->pool[i].lock);
        }

        airy_mtx_destroy(&g_ipc_client->pool_lock);
        AIRY_FREE(g_ipc_client->base_url);
        AIRY_FREE(g_ipc_client);
        g_ipc_client = NULL;
    }

    if (g_curl_initialized) {
        curl_global_cleanup();
        g_curl_initialized = 0;
    }

    airy_mtx_unlock(&g_init_lock);
}

int svc_rpc_call(const char *method, const char *params, char **out_result, uint32_t timeout_ms)
{
    if (!method || !out_result) {
        return SVC_ERR_INVALID_PARAM;
    }

    if (!g_ipc_client || !g_ipc_client->initialized) {
        return SVC_ERR_RPC;
    }

    *out_result = NULL;

    ipc_pool_entry_t *entry = pool_acquire(g_ipc_client);
    if (!entry) {
        return SVC_ERR_RPC;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);

    if (params) {

        do {
            CJSON_PARSE_GUARD(params_json, params, {
                cJSON_AddStringToObject(root, "params", params);
                break;
            });
            cJSON_AddItemToObject(root, "params", params_json);
            params_json = NULL;
        } while (0);
    }

    cJSON_AddNumberToObject(root, "id", (double)airy_time_ns());

    char *request = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!request) {
        pool_release(g_ipc_client, entry);
        return SVC_ERR_OUT_OF_MEMORY;
    }

    ipc_response_buffer_t response;
    if (buffer_init(&response) != 0) {
        AIRY_FREE(request);
        pool_release(g_ipc_client, entry);
        return SVC_ERR_OUT_OF_MEMORY;
    }

    if (timeout_ms == 0) {
        timeout_ms = g_ipc_client->default_timeout_ms;
    }

    int ret =
        do_rpc_call(entry, g_ipc_client->base_url, request, &response, timeout_ms, IPC_MAX_RETRIES);

    AIRY_FREE(request);
    request = NULL;

    if (ret != SVC_OK) {
        buffer_free(&response);
        pool_release(g_ipc_client, entry);
        return ret;
    }

    CJSON_PARSE_GUARD(resp_json, response.data, {
        buffer_free(&response);
        pool_release(g_ipc_client, entry);
        return SVC_ERR_RPC;
    });

    cJSON *error = cJSON_GetObjectItem(resp_json, "error");
    if (error) {

        buffer_free(&response);
        pool_release(g_ipc_client, entry);
        return SVC_ERR_RPC;
    }

    *out_result = response.data;

    pool_release(g_ipc_client, entry);
    return SVC_OK;
}

/**
 * @brief Set the default timeout
 */
int svc_ipc_set_timeout(uint32_t timeout_ms)
{
    if (!g_ipc_client || !g_ipc_client->initialized) {
        return SVC_ERR_RPC;
    }

    g_ipc_client->default_timeout_ms = timeout_ms;
    return SVC_OK;
}

/**
 * @brief Get the connection pool status
 */
int svc_ipc_get_pool_status(int *total, int *available)
{
    if (!g_ipc_client || !g_ipc_client->initialized) {
        return SVC_ERR_RPC;
    }

    if (total)
        *total = IPC_POOL_SIZE;

    if (available) {
        *available = 0;
        airy_mtx_lock(&g_ipc_client->pool_lock);
        for (int i = 0; i < IPC_POOL_SIZE; i++) {
            if (!g_ipc_client->pool[i].in_use) {
                (*available)++;
            }
        }
        airy_mtx_unlock(&g_ipc_client->pool_lock);
    }

    return SVC_OK;
}
