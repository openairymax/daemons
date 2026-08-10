// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file svc_model_defaults.c
 * @brief model.yaml global 段默认模型提取实现
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * libyaml 事件流解析。状态机：
 *   - pending_global：刚读到顶层键 "global"，等待其 mapping 开始
 *   - global_depth：global mapping 嵌套深度（global 自身为 1）
 * 仅提取 global 顶层（depth==1）的 default_model / default_provider，
 * 嵌套子段（如 default_retry:）与其余字段一律忽略。
 */

#include "svc_model_defaults.h"

#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_YAML
#include <yaml.h>
#endif

/* AIRY_STRNCPY_TERM 依赖：缓冲填满时强制置终结符 */
#ifndef AIRY_STRNCPY_TERM
#define AIRY_STRNCPY_TERM(dst, src, sz)          \
    do {                                         \
        snprintf((dst), (sz), "%s", (src));      \
        (dst)[(sz) - 1] = '\0';                  \
    } while (0)
#endif

int svc_model_defaults_from_yaml(const char *path,
                                 char *out_model, size_t model_sz,
                                 char *out_provider, size_t prov_sz)
{
    if (!path || !path[0])
        return AIRY_ERR_INVALID_PARAM;
    if (out_model && model_sz > 0)
        out_model[0] = '\0';
    if (out_provider && prov_sz > 0)
        out_provider[0] = '\0';

#ifndef HAVE_YAML
    return AIRY_ERR_NOT_SUPPORTED;
#else
    FILE *f = fopen(path, "rb");
    if (!f)
        return AIRY_ERR_IO;

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        return AIRY_ERR_NOT_SUPPORTED;
    }
    yaml_parser_set_input_file(&parser, f);

    yaml_event_t event;
    int pending_global = 0;   /* 刚读到顶层键 "global"，等待其 mapping */
    int global_depth = 0;     /* global mapping 嵌套深度（0 = 不在 global 内） */
    char key[128] = {0};
    int done = 0;

    while (!done) {
        if (!yaml_parser_parse(&parser, &event))
            break;
        switch (event.type) {
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
            if (pending_global) {
                global_depth = 1;
                pending_global = 0;
            } else if (global_depth > 0) {
                global_depth++;
                key[0] = '\0'; /* 嵌套 mapping：清空 key 状态避免串读 */
            }
            break;
        case YAML_MAPPING_END_EVENT:
            if (global_depth > 0) {
                global_depth--;
                key[0] = '\0';
            }
            break;
        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)event.data.scalar.value;
            if (!val)
                break;
            if (pending_global) {
                /* 顶层 "global" 后直接跟 scalar（而非 mapping）→ 值非 map，放弃 */
                pending_global = 0;
                key[0] = '\0';
            } else if (global_depth == 0) {
                if (strcmp(val, "global") == 0)
                    pending_global = 1;
            } else if (global_depth == 1) {
                if (key[0] == '\0') {
                    if ((out_model && strcmp(val, "default_model") == 0) ||
                        (out_provider && strcmp(val, "default_provider") == 0)) {
                        AIRY_STRNCPY_TERM(key, val, sizeof(key));
                    } else {
                        key[0] = '\0'; /* 不关心的键：跳过其值 */
                    }
                } else {
                    if (strcmp(key, "default_model") == 0 && out_model && model_sz > 0)
                        AIRY_STRNCPY_TERM(out_model, val, model_sz);
                    else if (strcmp(key, "default_provider") == 0 && out_provider &&
                             prov_sz > 0)
                        AIRY_STRNCPY_TERM(out_provider, val, prov_sz);
                    key[0] = '\0';
                }
            }
            /* global_depth > 1：嵌套子段字段，忽略 */
            break;
        }
        default:
            break;
        }
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(f);
    return 0;
#endif
}

/* ─── 简化 llm 段提取 ─────────────────────────────────────────────────── */

int svc_model_defaults_llm_from_yaml(const char *path,
                                     svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;
    /* 调用方先行清零；未找到 llm 段时保持初始值 */

#ifndef HAVE_YAML
    return AIRY_ERR_NOT_SUPPORTED;
#else
    FILE *f = fopen(path, "rb");
    if (!f)
        return AIRY_ERR_IO;

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        return AIRY_ERR_NOT_SUPPORTED;
    }
    yaml_parser_set_input_file(&parser, f);

    yaml_event_t event;
    int pending_llm = 0;   /* 刚读到顶层键 "llm"，等待其 mapping */
    int llm_depth = 0;     /* llm mapping 嵌套深度（0 = 不在 llm 内） */
    char key[128] = {0};
    int done = 0;

    while (!done) {
        if (!yaml_parser_parse(&parser, &event))
            break;
        switch (event.type) {
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
            if (pending_llm) {
                llm_depth = 1;
                pending_llm = 0;
            } else if (llm_depth > 0) {
                llm_depth++;
                key[0] = '\0';
            }
            break;
        case YAML_MAPPING_END_EVENT:
            if (llm_depth > 0) {
                llm_depth--;
                key[0] = '\0';
            }
            break;
        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)event.data.scalar.value;
            if (!val)
                break;
            if (pending_llm) {
                /* 顶层 "llm" 后直接跟 scalar（而非 mapping）→ 值非 map，放弃 */
                pending_llm = 0;
                key[0] = '\0';
            } else if (llm_depth == 0) {
                if (strcmp(val, "llm") == 0)
                    pending_llm = 1;
            } else if (llm_depth == 1) {
                if (key[0] == '\0') {
                    if (strcmp(val, "api_format") == 0 || strcmp(val, "base_url") == 0 ||
                        strcmp(val, "api_key_env") == 0 || strcmp(val, "model") == 0) {
                        AIRY_STRNCPY_TERM(key, val, sizeof(key));
                    } else {
                        key[0] = '\0';
                    }
                } else {
                    if (strcmp(key, "api_format") == 0) {
                        AIRY_STRNCPY_TERM(out->api_format, val, sizeof(out->api_format));
                    } else if (strcmp(key, "base_url") == 0) {
                        AIRY_STRNCPY_TERM(out->base_url, val, sizeof(out->base_url));
                    } else if (strcmp(key, "api_key_env") == 0) {
                        AIRY_STRNCPY_TERM(out->api_key_env, val, sizeof(out->api_key_env));
                    } else if (strcmp(key, "model") == 0) {
                        AIRY_STRNCPY_TERM(out->model, val, sizeof(out->model));
                    }
                    key[0] = '\0';
                }
            }
            /* llm_depth > 1：嵌套子段字段，忽略 */
            break;
        }
        default:
            break;
        }
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(f);
    return 0;
#endif
}

/* YAML 布尔标量规范化：true/yes/on/1 → 1；false/no/off/0 → 0 */
static int svc_yaml_bool(const char *val)
{
    if (!val || !*val)
        return -1;
    if (strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0 ||
        strcasecmp(val, "on") == 0 || strcmp(val, "1") == 0)
        return 1;
    if (strcasecmp(val, "false") == 0 || strcasecmp(val, "no") == 0 ||
        strcasecmp(val, "off") == 0 || strcmp(val, "0") == 0)
        return 0;
    return -1;
}

int svc_model_defaults_think_from_yaml(const char *path,
                                       svc_model_think_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;
    /* 调用方先行清零；未找到 think 段时保持初始值（enabled=1 由调用方设定） */

#ifndef HAVE_YAML
    return AIRY_ERR_NOT_SUPPORTED;
#else
    FILE *f = fopen(path, "rb");
    if (!f)
        return AIRY_ERR_IO;

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        return AIRY_ERR_NOT_SUPPORTED;
    }
    yaml_parser_set_input_file(&parser, f);

    yaml_event_t event;
    int pending_think = 0;   /* 刚读到顶层键 "think"，等待其 mapping */
    int think_depth = 0;     /* think mapping 嵌套深度（0 = 不在 think 内） */
    char key[128] = {0};
    int done = 0;

    while (!done) {
        if (!yaml_parser_parse(&parser, &event))
            break;
        switch (event.type) {
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
            if (pending_think) {
                think_depth = 1;
                pending_think = 0;
            } else if (think_depth > 0) {
                think_depth++;
                key[0] = '\0';
            }
            break;
        case YAML_MAPPING_END_EVENT:
            if (think_depth > 0) {
                think_depth--;
                key[0] = '\0';
            }
            break;
        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)event.data.scalar.value;
            if (!val)
                break;
            if (pending_think) {
                /* 顶层 "think" 后直接跟 scalar（而非 mapping）→ 值非 map，放弃 */
                pending_think = 0;
                key[0] = '\0';
            } else if (think_depth == 0) {
                if (strcmp(val, "think") == 0)
                    pending_think = 1;
            } else if (think_depth == 1) {
                if (key[0] == '\0') {
                    /* 键位置：仅登记关注的键，其余忽略其值 */
                    if (strcmp(val, "enabled") == 0 || strcmp(val, "think2_slow_model") == 0 ||
                        strcmp(val, "think1_fast_model") == 0 ||
                        strcmp(val, "think1_prof_model") == 0 ||
                        strcmp(val, "timeout_ms") == 0) {
                        AIRY_STRNCPY_TERM(key, val, sizeof(key));
                    } else {
                        key[0] = '\0';
                    }
                } else {
                    if (strcmp(key, "enabled") == 0) {
                        int b = svc_yaml_bool(val);
                        if (b >= 0)
                            out->enabled = b;
                    } else if (strcmp(key, "think2_slow_model") == 0) {
                        AIRY_STRNCPY_TERM(out->think2_slow_model, val,
                                          sizeof(out->think2_slow_model));
                    } else if (strcmp(key, "think1_fast_model") == 0) {
                        AIRY_STRNCPY_TERM(out->think1_fast_model, val,
                                          sizeof(out->think1_fast_model));
                    } else if (strcmp(key, "think1_prof_model") == 0) {
                        AIRY_STRNCPY_TERM(out->think1_prof_model, val,
                                          sizeof(out->think1_prof_model));
                    } else if (strcmp(key, "timeout_ms") == 0) {
                        long t = strtol(val, NULL, 10);
                        if (t > 0)
                            out->timeout_ms = (uint32_t)t;
                    }
                    key[0] = '\0';
                }
            }
            /* think_depth > 1：嵌套子段字段，忽略 */
            break;
        }
        default:
            break;
        }
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(f);
    return 0;
#endif
}
