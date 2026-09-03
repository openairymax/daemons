// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file provider.c
 * @brief Provider 公共基础设施：base ctx 生命周期与 API key 管理。
 *
 * 域拆分（2026-08-27：原 909 行 → 4 文件，公共 API 与行为不变）：
 * - provider.c       base ctx 初始化 / api_key 解析 / secrets.env 热重载
 * - provider_http.c  HTTP 非流式 POST（provider_http_post / resp_free）
 * - provider_codec.c OpenAI 兼容请求组装 / 响应解析
 * - provider_stream.c SSE 流式传输 / 流式控制帧 / 增长缓冲
 */

#include "provider.h"
#include "svc_logger.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* macOS 严格 feature 宏（-std=c99 等）下 <string.h> 不声明 explicit_bzero，
 * Windows UCRT 亦无；此处 <string.h> 已包含（若有系统声明已就位，宏不会
 * 与其碰撞），随后所有调用点统一展开为 volatile 擦除，保证敏感内存清零
 * 不被优化器消去。 */
#ifndef explicit_bzero
static inline void airy_provider_explicit_bzero(void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (n-- > 0) {
        *p++ = 0;
    }
}
#define explicit_bzero(s, n) airy_provider_explicit_bzero((s), (n))
#endif


static airy_mtx_t g_secrets_refresh_lock;
static bool g_secrets_lock_inited = false;

static void secrets_lock_ensure(void)
{
    if (!g_secrets_lock_inited) {
        airy_mtx_init(&g_secrets_refresh_lock);
        g_secrets_lock_inited = true;
    }
}

/**
 * @brief Read the value of the given key from $AIRY_HOME/config/secrets.env.
 * @return Dynamically-allocated string (caller AIRY_FREEs), NULL if not
 *         found or read failed
 */
static char *secrets_env_read(const char *key_name)
{
    if (!key_name || !key_name[0])
        return NULL;

    char path[1024];
    snprintf(path, sizeof(path), "%s/secrets.env", airy_config_dir());

    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    char line[1024];
    char *found = NULL;
    while (fgets(line, sizeof(line), f)) {

        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;

        char *key_end = eq;
        while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t'))
            key_end--;
        size_t key_len = (size_t)(key_end - p);
        if (key_len == 0 || key_len != strlen(key_name) || strncmp(p, key_name, key_len) != 0)
            continue;

        char *v = eq + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        size_t vlen = strlen(v);
        while (vlen > 0 && (v[vlen - 1] == '\n' || v[vlen - 1] == '\r' || v[vlen - 1] == ' ' ||
                            v[vlen - 1] == '\t'))
            vlen--;
        if (vlen > 0 && v[0] == '"' && v[vlen - 1] == '"') {
            v++;
            vlen -= 2;
        }
        if (vlen > 0) {
            found = (char *)AIRY_MALLOC(vlen + 1);
            if (found) {
                __builtin_memcpy(found, v, vlen);
                found[vlen] = '\0';
            }
        }
        break;
    }

    explicit_bzero(line, sizeof(line));
    fclose(f);
    return found;
}

/**
 * @brief Hot-reload api_key on request: re-resolve on every request (env var
 *        first, secrets.env as fallback)
 *
 * Design intent: ordinary users need not restart llm_d — fill the API key in
 * $AIRY_HOME/config/secrets.env and save; the next request picks it up
 * automatically; later key changes take effect immediately.
 * Priority: environment variable (secrets.env sourced at bootstrap) >
 * secrets.env file.
 * Security: key values are never written to logs (only the env name and
 * source are recorded); temporary buffers are zeroed after use.
 */
void provider_refresh_api_key(provider_base_ctx_t *base_ctx)
{
    if (!base_ctx || base_ctx->api_key_env[0] == '\0')
        return;

    const char *env_val = getenv(base_ctx->api_key_env);
    const char *new_key = (env_val && env_val[0]) ? env_val : NULL;
    char *file_val = NULL;
    if (!new_key) {
        file_val = secrets_env_read(base_ctx->api_key_env);
        new_key = file_val;
    }
    if (!new_key || !new_key[0])
        return;

    secrets_lock_ensure();
    airy_mtx_lock(&g_secrets_refresh_lock);

    if (strcmp(base_ctx->api_key, new_key) != 0) {
        size_t n = strlen(new_key);
        if (n < sizeof(base_ctx->api_key)) {
            explicit_bzero(base_ctx->api_key, sizeof(base_ctx->api_key));
            __builtin_memcpy(base_ctx->api_key, new_key, n + 1);
            SVC_LOG_INFO("C-L02: PROVIDER: KEY-RELOAD api_key_env=%s source=%s",
                         base_ctx->api_key_env, env_val ? "env" : "secrets.env");
        }
    }
    airy_mtx_unlock(&g_secrets_refresh_lock);

    if (file_val) {
        explicit_bzero(file_val, strlen(file_val));
        AIRY_FREE(file_val);
    }
}

static const char *resolve_api_key(const char *api_key)
{
    if (!api_key || api_key[0] == '\0') {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (strncmp(api_key, "env:", 4) == 0) {
        const char *env_name = api_key + 4;
        const char *env_val = getenv(env_name);
        if (env_val && env_val[0]) {
            return env_val;
        }
        SVC_LOG_WARN("Environment variable '%s' not set or empty", env_name);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
    }

    return api_key;
}

static const char *fallback_env_key(const char *provider_name)
{
    if (!provider_name) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (strcmp(provider_name, "openai") == 0)
        return getenv("OPENAI_API_KEY");
    if (strcmp(provider_name, "anthropic") == 0)
        return getenv("ANTHROPIC_API_KEY");
    if (strcmp(provider_name, "deepseek") == 0)
        return getenv("DEEPSEEK_API_KEY");
    if (strcmp(provider_name, "google") == 0)
        return getenv("GOOGLE_AI_API_KEY");
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

static const char *guess_provider_from_url(const char *url)
{
    if (!url) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (strstr(url, "openai.com"))
        return "openai";
    if (strstr(url, "anthropic.com"))
        return "anthropic";
    if (strstr(url, "deepseek.com"))
        return "deepseek";
    if (strstr(url, "googleapis.com"))
        return "google";
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

void provider_base_init(provider_base_ctx_t *base_ctx, const char *api_key, const char *api_base,
                        const char *organization, double timeout_sec, int max_retries,
                        const char *default_base)
{
    if (!base_ctx)
        return;

    __builtin_memset(base_ctx, 0, sizeof(provider_base_ctx_t));

    /* Record the api_key_env name (for secrets.env hot-reload on request).
     * Prefer extracting from the "env:NAME" prefix; otherwise infer the
     * standard env var name from the provider/base_url. */
    base_ctx->api_key_env[0] = '\0';
    if (api_key && strncmp(api_key, "env:", 4) == 0) {
        const char *env_name = api_key + 4;
        size_t env_len = strlen(env_name);

        if (env_len > 0 && env_len < sizeof(base_ctx->api_key_env)) {
            __builtin_memcpy(base_ctx->api_key_env, env_name, env_len + 1);
        }
    } else {
        const char *guessed = guess_provider_from_url(api_base ? api_base : default_base);
        if (guessed) {
            const char *env_name = NULL;
            if (strcmp(guessed, "openai") == 0)
                env_name = "OPENAI_API_KEY";
            else if (strcmp(guessed, "anthropic") == 0)
                env_name = "ANTHROPIC_API_KEY";
            else if (strcmp(guessed, "deepseek") == 0)
                env_name = "DEEPSEEK_API_KEY";
            else if (strcmp(guessed, "google") == 0)
                env_name = "GOOGLE_AI_API_KEY";
            if (env_name)
                AIRY_STRNCPY_TERM(base_ctx->api_key_env, env_name, sizeof(base_ctx->api_key_env));
        }
    }

    const char *resolved_key = resolve_api_key(api_key);
    if (!resolved_key || resolved_key[0] == '\0') {
        const char *guessed = guess_provider_from_url(api_base ? api_base : default_base);
        resolved_key = fallback_env_key(guessed);
    }

    if (resolved_key) {
        size_t key_len = strlen(resolved_key);
        if (key_len < sizeof(base_ctx->api_key)) {
            __builtin_memcpy(base_ctx->api_key, resolved_key, key_len + 1);
        }
    }

    if (api_base) {
        size_t base_len = strlen(api_base);
        if (base_len < sizeof(base_ctx->api_base)) {
            __builtin_memcpy(base_ctx->api_base, api_base, base_len + 1);
        }
    } else if (default_base) {
        size_t default_len = strlen(default_base);
        if (default_len < sizeof(base_ctx->api_base)) {
            __builtin_memcpy(base_ctx->api_base, default_base, default_len + 1);
        }
    }

    if (organization) {
        size_t org_len = strlen(organization);
        if (org_len < sizeof(base_ctx->organization)) {
            __builtin_memcpy(base_ctx->organization, organization, org_len + 1);
        }
    }

    base_ctx->timeout_sec = timeout_sec > 0 ? timeout_sec : 120.0;
    base_ctx->max_retries = max_retries > 0 ? max_retries : 3;

    SVC_LOG_INFO("C-L02: PROVIDER: BASE-INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d",
                 base_ctx->api_base[0] ? base_ctx->api_base : "(none)", base_ctx->timeout_sec,
                 base_ctx->max_retries, base_ctx->api_key[0] ? 1 : 0);
}

provider_base_ctx_t *provider_base_ctx(provider_ctx_t *ctx)
{
    /* 约定：所有 provider 的 ctx 首字段均为 provider_base_ctx_t base */
    return (provider_base_ctx_t *)ctx;
}
