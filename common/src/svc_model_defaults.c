// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
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
#include <string.h>

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
