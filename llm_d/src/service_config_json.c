// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_json.c
 * @brief LLM service config-loading domain: JSON model-config loading
 *        (split from service_config.c, 2026-08-27).
 *
 * 2026-08-27 域拆分（service_config.c 1159 行 → 2 文件）：
 *   - service_config.c      配置解析核心（JSON/YAML 全局配置 + YAML 模型配置 + 定价规则）
 *   - service_config_json.c JSON 模型配置加载（provider/model 解析）
 */

#include "airy_memory.h"
#include "daemon_defaults.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_service_internal.h"

/**
 * @brief Load model config from JSON (providers array).
 * @param config_path Config file path (.json)
 * @param out_providers Output provider array (caller frees)
 * @param out_count Output provider count
 * @return 0 on success, non-zero on failure
 */
int svc_load_model_config_json(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count)
{
    if (!config_path || !out_providers || !out_count) {
        SVC_LOG_ERROR(
            "C-L02: SVC: MODEL-CONFIG-FAIL NULL parameter, STACK: svc_load_model_config_json");
        return AIRY_ERR_INVALID_PARAM;
    }

    *out_providers = NULL;
    *out_count = 0;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN cannot open file, STACK: svc_load_model_config_json");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = (char *)AIRY_MALLOC((size_t)len + 1);
    if (!content) {
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL malloc, STACK: svc_load_model_config_json");
        fclose(f);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t read_len = fread(content, 1, (size_t)len, f);
    content[read_len] = '\0';
    fclose(f);

    CJSON_PARSE_GUARD(root, content, {
        AIRY_FREE(content);
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN parse failed, STACK: svc_load_model_config_json");
        return 0;
    });
    AIRY_FREE(content);

    cJSON *providers_arr = cJSON_GetObjectItem(root, "providers");
    if (!providers_arr || !cJSON_IsArray(providers_arr)) {

        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN no providers, STACK: svc_load_model_config_json");
        return 0;
    }

    int n = cJSON_GetArraySize(providers_arr);
    if (n <= 0) {

        return 0;
    }

    provider_config_t *result =
        (provider_config_t *)AIRY_CALLOC((size_t)n + 1, sizeof(provider_config_t));
    if (!result) {
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL calloc, STACK: svc_load_model_config_json");

        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t valid_count = 0;
    for (int i = 0; i < n; ++i) {
        cJSON *pitem = cJSON_GetArrayItem(providers_arr, i);
        cJSON *pname = cJSON_GetObjectItem(pitem, "name");
        cJSON *pkey_env = cJSON_GetObjectItem(pitem, "api_key_env");
        cJSON *pbase = cJSON_GetObjectItem(pitem, "base_url");
        cJSON *ptimeout = cJSON_GetObjectItem(pitem, "timeout_sec");
        cJSON *pretries = cJSON_GetObjectItem(pitem, "max_retries");
        cJSON *pmodels = cJSON_GetObjectItem(pitem, "models");

        if (!cJSON_IsString(pname))
            continue;

        provider_config_t *pcfg = &result[valid_count];
        pcfg->name = AIRY_STRDUP(pname->valuestring);

        if (cJSON_IsString(pkey_env) && pkey_env->valuestring[0]) {
            size_t env_len = strlen(pkey_env->valuestring);
            char *key_buf = (char *)AIRY_MALLOC(4 + env_len + 1);
            if (key_buf) {
                __builtin_memcpy(key_buf, "env:", 4);
                __builtin_memcpy(key_buf + 4, pkey_env->valuestring, env_len + 1);
                pcfg->api_key = key_buf;
            }
        }

        if (cJSON_IsString(pbase))
            pcfg->api_base = AIRY_STRDUP(pbase->valuestring);
        if (cJSON_IsNumber(ptimeout))
            pcfg->timeout_sec = ptimeout->valuedouble;
        if (cJSON_IsNumber(pretries))
            pcfg->max_retries = pretries->valueint;

        if (cJSON_IsArray(pmodels)) {
            int mcount = cJSON_GetArraySize(pmodels);
            char **marr = (char **)AIRY_CALLOC((size_t)mcount + 1, sizeof(char *));
            if (marr) {
                for (int j = 0; j < mcount; ++j) {
                    cJSON *mitem = cJSON_GetArrayItem(pmodels, j);
                    if (cJSON_IsString(mitem))
                        marr[j] = AIRY_STRDUP(mitem->valuestring);
                }
                marr[mcount] = NULL;
                pcfg->models = marr;
            }
        }
        valid_count++;
    }

    *out_providers = result;
    *out_count = valid_count;
    SVC_LOG_INFO("C-L02: SVC: MODEL-CONFIG-OK providers=%zu, STACK: svc_load_model_config_json",
                 valid_count);
    return AIRY_OK;
}
