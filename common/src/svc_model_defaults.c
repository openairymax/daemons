// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_model_defaults.c
 * @brief Default-model extraction implementation for the model.yaml global section.
 *
 * libyaml event-stream parsing. State machine:
 *   - pending_global: just read the top-level key "global", waiting for its mapping to start
 *   - global_depth: nesting depth of the global mapping (global itself is 1)
 * Only the top-level (depth==1) default_model / default_provider of global
 * are extracted; nested subsections (e.g. default_retry:) and other fields
 * are ignored.
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

#ifndef AIRY_STRNCPY_TERM
#define AIRY_STRNCPY_TERM(dst, src, sz)     \
    do {                                    \
        snprintf((dst), (sz), "%s", (src)); \
        (dst)[(sz) - 1] = '\0';             \
    } while (0)
#endif

int svc_model_defaults_from_yaml(const char *path, char *out_model, size_t model_sz,
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
    int pending_global = 0;
    int global_depth = 0;
    char key[128] = {0};
    int done = 0;
    /* v2 表格格式（2026-08-26）：顶层 default_model/default_provider 与
     * global 段等价（SSoT 精简后不再需要 global 嵌套段）。 */
    int pending_top = 0;

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
                key[0] = '\0';
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

                pending_global = 0;
                key[0] = '\0';
            } else if (global_depth == 0) {
                if (pending_top) {
                    if (strcmp(key, "default_model") == 0 && out_model && model_sz > 0)
                        AIRY_STRNCPY_TERM(out_model, val, model_sz);
                    else if (strcmp(key, "default_provider") == 0 && out_provider && prov_sz > 0)
                        AIRY_STRNCPY_TERM(out_provider, val, prov_sz);
                    pending_top = 0;
                    key[0] = '\0';
                } else if (strcmp(val, "global") == 0) {
                    pending_global = 1;
                } else if (strcmp(val, "default_model") == 0 ||
                           strcmp(val, "default_provider") == 0) {
                    AIRY_STRNCPY_TERM(key, val, sizeof(key));
                    pending_top = 1;
                }
            } else if (global_depth == 1) {
                if (key[0] == '\0') {
                    if ((out_model && strcmp(val, "default_model") == 0) ||
                        (out_provider && strcmp(val, "default_provider") == 0)) {
                        AIRY_STRNCPY_TERM(key, val, sizeof(key));
                    } else {
                        key[0] = '\0';
                    }
                } else {
                    if (strcmp(key, "default_model") == 0 && out_model && model_sz > 0)
                        AIRY_STRNCPY_TERM(out_model, val, model_sz);
                    else if (strcmp(key, "default_provider") == 0 && out_provider && prov_sz > 0)
                        AIRY_STRNCPY_TERM(out_provider, val, prov_sz);
                    key[0] = '\0';
                }
            }

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

int svc_model_defaults_llm_from_yaml(const char *path, svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

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
    int pending_llm = 0;
    int llm_depth = 0;
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

/* v2 表格格式（2026-08-26）：读取顶层 models 表首个条目的主连接字段
 * （api_format / base_url / api_key_env / model_id），供 llm_d / gateway_d
 * 在 llm: 段缺省时取默认连接。models[0].model_id 即默认模型名。 */
int svc_model_defaults_models0_from_yaml(const char *path, svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

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
    int in_models = 0;
    int item_depth = 0;
    int done = 0;
    int captured = 0;
    char key[128] = {0};
    int have_key = 0;

    while (!done && !captured) {
        if (!yaml_parser_parse(&parser, &event))
            break;
        switch (event.type) {
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
            if (in_models) {
                item_depth++;
                if (item_depth == 1)
                    have_key = 0;
            }
            break;
        case YAML_MAPPING_END_EVENT:
            if (in_models) {
                item_depth--;
                if (item_depth == 0)
                    captured = 1;
            }
            break;
        case YAML_SEQUENCE_START_EVENT:
            break;
        case YAML_SEQUENCE_END_EVENT:
            break;
        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)event.data.scalar.value;
            if (!val)
                break;
            if (!in_models && item_depth == 0 && have_key == 0) {
                if (strcmp(val, "models") == 0)
                    in_models = 1;
                break;
            }
            if (in_models && item_depth == 1) {
                if (!have_key) {
                    AIRY_STRNCPY_TERM(key, val, sizeof(key));
                    have_key = 1;
                } else {
                    if (strcmp(key, "api_format") == 0)
                        AIRY_STRNCPY_TERM(out->api_format, val, sizeof(out->api_format));
                    else if (strcmp(key, "base_url") == 0)
                        AIRY_STRNCPY_TERM(out->base_url, val, sizeof(out->base_url));
                    else if (strcmp(key, "api_key_env") == 0)
                        AIRY_STRNCPY_TERM(out->api_key_env, val, sizeof(out->api_key_env));
                    else if (strcmp(key, "model_id") == 0)
                        AIRY_STRNCPY_TERM(out->model, val, sizeof(out->model));
                    have_key = 0;
                }
            }
            break;
        }
        default:
            break;
        }
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(f);
    return captured ? 0 : AIRY_ERR_NOT_FOUND;
#endif
}

static int svc_yaml_bool(const char *val)
{
    if (!val || !*val)
        return -1;
    if (strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0 ||
        strcmp(val, "1") == 0)
        return 1;
    if (strcasecmp(val, "false") == 0 || strcasecmp(val, "no") == 0 ||
        strcasecmp(val, "off") == 0 || strcmp(val, "0") == 0)
        return 0;
    return -1;
}

int svc_model_defaults_think_from_yaml(const char *path, svc_model_think_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

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
    int pending_think = 0;
    int think_depth = 0;
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

                pending_think = 0;
                key[0] = '\0';
            } else if (think_depth == 0) {
                if (strcmp(val, "think") == 0)
                    pending_think = 1;
            } else if (think_depth == 1) {
                if (key[0] == '\0') {

                    if (strcmp(val, "enabled") == 0 || strcmp(val, "think2_slow_model") == 0 ||
                        strcmp(val, "think1_fast_model") == 0 ||
                        strcmp(val, "think1_prof_model") == 0 || strcmp(val, "timeout_ms") == 0) {
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
