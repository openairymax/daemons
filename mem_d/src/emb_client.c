// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file emb_client.c
 * @brief Memory 服务可选 embedding 后端客户端实现（libcurl + cJSON）
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 实现细节：
 * - 配置：AIRY_MEM_EMBEDDING_URL（base url）+ AIRY_MEM_EMBEDDING_KEY（可选 Bearer）
 * - 请求：POST {url}/embeddings，body={"model":"text-embedding-3-small","input":["<text>"]}
 * - 响应解析：data[0].embedding 浮点数组
 * - 降级：任何失败（HTTP 非 200 / 超时 / JSON 解析失败）标记 unhealthy 并记录冷却时间，
 *   冷却期（默认 60s，可用 AIRY_MEM_EMB_RETRY_SECONDS 调整）内不再发起网络请求，
 *   由上层自动降级为 TF-IDF 检索；冷却期后自动重试恢复
 */

#include "emb_client.h"

#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIRY_HAS_CURL
#include <curl/curl.h>
#endif

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

/* 默认失败重试冷却期（毫秒） */
#define MEM_EMB_RETRY_AFTER_MS 60000
/* 默认 embedding 模型 */
#define MEM_EMB_MODEL "text-embedding-3-small"
/* HTTP 超时（秒） */
#define MEM_EMB_TIMEOUT_SEC 10
#define MEM_EMB_CONNECT_TIMEOUT_SEC 5

/* curl 全局初始化只执行一次（curl_global_init 非线程安全） */
static int g_curl_lock_ready = 0;
static int g_curl_ready = 0;
static airy_mtx_t g_curl_lock;

#ifdef AIRY_HAS_CURL

static void mem_curl_global_init_once(void)
{
    /* 惰性初始化锁（mem_service_create 在主线程调用，竞争风险可忽略） */
    if (!g_curl_lock_ready) {
        airy_mtx_init(&g_curl_lock);
        g_curl_lock_ready = 1;
    }
    airy_mtx_lock(&g_curl_lock);
    if (!g_curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK)
            g_curl_ready = 1;
    }
    airy_mtx_unlock(&g_curl_lock);
}

/* libcurl 写回调：累积响应体 */
typedef struct {
    char *buf;
    size_t len;
} mem_emb_resp_t;

static size_t mem_emb_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    mem_emb_resp_t *r = (mem_emb_resp_t *)userdata;
    size_t n = size * nmemb;
    if (!r || n == 0)
        return 0;
    char *nb = (char *)AIRY_REALLOC(r->buf, r->len + n + 1);
    if (!nb)
        return 0;
    AIRY_MEMCPY(nb + r->len, ptr, n);
    r->buf = nb;
    r->len += n;
    r->buf[r->len] = '\0';
    return n;
}

#endif /* AIRY_HAS_CURL */

int mem_emb_client_init(mem_emb_client_t *client)
{
    if (!client)
        return AIRY_ERR_INVALID_PARAM;
    AIRY_MEMSET(client, 0, sizeof(*client));
    client->retry_after_ms = MEM_EMB_RETRY_AFTER_MS;

#ifndef AIRY_HAS_CURL
    SVC_LOG_DEBUG("mem_d embedding backend disabled (libcurl unavailable)");
    return AIRY_SUCCESS;
#else
    const char *url = getenv("AIRY_MEM_EMBEDDING_URL");
    const char *key = getenv("AIRY_MEM_EMBEDDING_KEY");
    if (!url || !url[0]) {
        SVC_LOG_DEBUG("mem_d embedding backend disabled (AIRY_MEM_EMBEDDING_URL unset)");
        return AIRY_SUCCESS;
    }

    client->url = AIRY_STRDUP(url);
    client->api_key = (key && key[0]) ? AIRY_STRDUP(key) : NULL;
    if (!client->url || (key && key[0] && !client->api_key)) {
        AIRY_FREE(client->url);
        AIRY_FREE(client->api_key);
        AIRY_MEMSET(client, 0, sizeof(*client));
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 冷却期可配置 */
    const char *rs = getenv("AIRY_MEM_EMB_RETRY_SECONDS");
    if (rs && rs[0]) {
        long v = strtol(rs, NULL, 10);
        if (v > 0)
            client->retry_after_ms = (uint64_t)v * 1000;
    }

    client->enabled = 1;
    client->healthy = 1;
    client->last_fail_time = 0;

    /* 仅当真正使用 curl 前初始化一次 */
    mem_curl_global_init_once();
    SVC_LOG_INFO("mem_d embedding backend enabled (url=%s)", client->url);
    return AIRY_SUCCESS;
#endif
}

void mem_emb_client_destroy(mem_emb_client_t *client)
{
    if (!client)
        return;
    AIRY_FREE(client->url);
    AIRY_FREE(client->api_key);
    AIRY_MEMSET(client, 0, sizeof(*client));
}

int mem_emb_should_try(const mem_emb_client_t *client)
{
    if (!client || !client->enabled)
        return 0;
    if (client->healthy)
        return 1;
    uint64_t now = airy_time_ms();
    if (now < client->last_fail_time) /* 时钟回拨保护 */
        return 0;
    return (now - client->last_fail_time) >= client->retry_after_ms;
}

#ifdef AIRY_HAS_CURL

/* 标记调用失败：进入冷却期（上层随后降级 TF-IDF） */
static void mem_emb_mark_fail(mem_emb_client_t *client)
{
    client->healthy = 0;
    client->last_fail_time = airy_time_ms();
}

int mem_emb_embed(mem_emb_client_t *client, const char *text,
                  float **out_vec, size_t *out_dim)
{
    if (!out_vec || !out_dim)
        return AIRY_ERR_INVALID_PARAM;
    *out_vec = NULL;
    *out_dim = 0;
    if (!client || !client->enabled || !text)
        return AIRY_ERR_INVALID_PARAM;

#ifdef AIRY_HAS_CJSON
    /* ---- 构造请求体 ---- */
    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_CreateArray();
    if (!root || !input) {
        if (root) cJSON_Delete(root);
        if (input) cJSON_Delete(input);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    cJSON_AddStringToObject(root, "model", MEM_EMB_MODEL);
    cJSON_AddItemToArray(input, cJSON_CreateString(text));
    cJSON_AddItemToObject(root, "input", input);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body)
        return AIRY_ERR_OUT_OF_MEMORY;

    /* ---- 拼接 URL：{base}/embeddings ---- */
    size_t url_len = strlen(client->url) + strlen("/embeddings") + 1;
    char *url = (char *)AIRY_MALLOC(url_len);
    if (!url) {
        cJSON_free(body);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    snprintf(url, url_len, "%s/embeddings", client->url);

    /* ---- 发起 HTTP POST ---- */
    CURL *curl = curl_easy_init();
    if (!curl) {
        AIRY_FREE(url);
        cJSON_free(body);
        mem_emb_mark_fail(client);
        return AIRY_ERR_SYS_RESOURCE;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (client->api_key) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", client->api_key);
        headers = curl_slist_append(headers, auth);
    }

    mem_emb_resp_t resp = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_emb_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, MEM_EMB_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, MEM_EMB_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    AIRY_FREE(url);
    cJSON_free(body);

    if (rc != CURLE_OK) {
        SVC_LOG_WARN("mem_d embedding request failed (curl=%d)", (int)rc);
        AIRY_FREE(resp.buf);
        mem_emb_mark_fail(client);
        return AIRY_ERR_LLM_PROVIDER_FAIL;
    }
    if (http_code != 200 || !resp.buf) {
        SVC_LOG_WARN("mem_d embedding request failed (http=%ld)", http_code);
        AIRY_FREE(resp.buf);
        mem_emb_mark_fail(client);
        return AIRY_ERR_LLM_PROVIDER_FAIL;
    }

    /* ---- 解析响应：data[0].embedding ---- */
    cJSON *r = cJSON_Parse(resp.buf);
    AIRY_FREE(resp.buf);
    if (!r) {
        mem_emb_mark_fail(client);
        return AIRY_ERR_LLM_PARSE_RESP;
    }
    cJSON *data = cJSON_GetObjectItem(r, "data");
    cJSON *first = (data && cJSON_IsArray(data)) ? cJSON_GetArrayItem(data, 0) : NULL;
    cJSON *emb = first ? cJSON_GetObjectItem(first, "embedding") : NULL;
    if (!cJSON_IsArray(emb)) {
        cJSON_Delete(r);
        mem_emb_mark_fail(client);
        return AIRY_ERR_LLM_PARSE_RESP;
    }

    int n = cJSON_GetArraySize(emb);
    if (n <= 0) {
        cJSON_Delete(r);
        mem_emb_mark_fail(client);
        return AIRY_ERR_LLM_EMPTY_RESP;
    }
    float *vec = (float *)AIRY_MALLOC((size_t)n * sizeof(float));
    if (!vec) {
        cJSON_Delete(r);
        mem_emb_mark_fail(client);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    for (int k = 0; k < n; k++) {
        cJSON *it = cJSON_GetArrayItem(emb, k);
        vec[k] = (it && cJSON_IsNumber(it)) ? (float)it->valuedouble : 0.0f;
    }
    cJSON_Delete(r);

    *out_vec = vec;
    *out_dim = (size_t)n;
    client->healthy = 1;
    return AIRY_SUCCESS;
#else
    (void)client;
    (void)text;
    return AIRY_ERR_NOT_SUPPORTED;
#endif
}

#else /* !AIRY_HAS_CURL */

int mem_emb_embed(mem_emb_client_t *client, const char *text,
                  float **out_vec, size_t *out_dim)
{
    (void)client;
    (void)text;
    if (out_vec)
        *out_vec = NULL;
    if (out_dim)
        *out_dim = 0;
    return AIRY_ERR_NOT_SUPPORTED;
}

#endif /* AIRY_HAS_CURL */

float mem_emb_cosine(const float *a, const float *b, size_t dim)
{
    if (!a || !b || dim == 0)
        return 0.0f;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    if (na == 0.0 || nb == 0.0)
        return 0.0f;
    double cos = dot / (sqrt(na) * sqrt(nb));
    if (cos < 0.0)
        cos = 0.0;
    if (cos > 1.0)
        cos = 1.0;
    return (float)cos;
}
