// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file secrets.c
 * @brief Provider 域密钥机制：secrets.env 读取、api_key 热重载、解析与映射。
 *
 * B16-S3 c3：自 provider.c 收敛。六份逐字重复的 explicit_bzero shim 归一为
 * secrets.h 的唯一实现；api_key 生命周期（env 展开、secrets.env 热重载、
 * 厂商 env 名映射）集中于此，适配层禁止自持副本。密钥值永不写日志（仅记
 * env 名与来源），临时缓冲用后即擦。
 */

#include "airy_memory.h"
#include "error.h"
#include "secrets.h"
#include "svc_logger.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

const char *sec_resolve_key(const char *api_key)
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

const char *sec_guess_provider(const char *url)
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

const char *sec_env_for_provider(const char *name)
{
    if (!name) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (strcmp(name, "openai") == 0)
        return "OPENAI_API_KEY";
    if (strcmp(name, "anthropic") == 0)
        return "ANTHROPIC_API_KEY";
    if (strcmp(name, "deepseek") == 0)
        return "DEEPSEEK_API_KEY";
    if (strcmp(name, "google") == 0)
        return "GOOGLE_AI_API_KEY";
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}
