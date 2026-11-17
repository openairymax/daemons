// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file review_schema.c
 * @brief C-4 双思考 B 侧：判决 schema 校验与提示词装配（纯函数）。
 *
 * 与 review_svc.c（RPC 域）分离的原因：服务面取值路径与黄金集必须
 * 共用同一份解析与提示词实现，测试只链接机制库即可复跑，不拉起 daemon。
 */

#include "review/review_svc.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REVIEW_JSON_MAX 1024
#define REVIEW_BODY_MAX 1024

int rv_schema_mode(void)
{
    const char *m = getenv("AIRY_REVIEW_SCHEMA");
    return (m && strcmp(m, "warn") == 0) ? 1 : 0;
}

/* 只接受数值 0/1 + 非空 reason；思考前缀/后缀允许存在，但截取出的对象
 * 必须完整可解析（缺右括号、字段类型错、越界值一律判不合法）。 */
int rv_schema_parse(const char *content, const char *key, int *raw,
                    char *reason, size_t sz)
{
    if (!content || !key || !key[0] || !raw)
        return 0;
    const char *b = strchr(content, '{');
    const char *e = strrchr(content, '}');
    if (!b || !e || e <= b)
        return 0;
    size_t n = (size_t)(e - b) + 1;
    if (n >= REVIEW_JSON_MAX)
        return 0;

    char buf[REVIEW_JSON_MAX];
    size_t i = 0;
    for (; i < n; ++i)
        buf[i] = b[i];
    buf[i] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        return 0;
    int ok = 0;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "reason");
    if (cJSON_IsNumber(v) && (v->valuedouble == 0.0 || v->valuedouble == 1.0) &&
        cJSON_IsString(r) && r->valuestring && r->valuestring[0] &&
        strlen(r->valuestring) <= 200) {
        *raw = (int)v->valuedouble;
        if (reason && sz > 0)
            snprintf(reason, sz, "%s", r->valuestring);
        ok = 1;
    }
    cJSON_Delete(root);
    return ok;
}

int rv_prompt_t2(char *buf, size_t sz, const char *goal, const char *sigs,
                 const char *body, const char *json)
{
    if (!buf || sz == 0)
        return -1;
    const char *b = (body && body[0]) ? body : "(无)";
    size_t bl = strlen(b);
    int take = (int)bl;
    const char *cut = "";
    if (bl > REVIEW_BODY_MAX) {
        take = REVIEW_BODY_MAX;
        cut = "\n[正文已截断]";
    }
    snprintf(buf, sz,
             "node_goal: %s\noutput_signatures: %s\noutput_body: %.*s%s\n"
             "output_json: %s",
             (goal && goal[0]) ? goal : "(无)", (sigs && sigs[0]) ? sigs : "(无)", take,
             b, cut, (json && json[0]) ? json : "(无)");
    return 0;
}

int rv_prompt_t1f(char *buf, size_t sz, const char *gate, int drift,
                  const char *goal, const char *body, const char *json)
{
    if (!buf || sz == 0)
        return -1;
    const char *b = (body && body[0]) ? body : "(无)";
    size_t bl = strlen(b);
    int take = (int)bl;
    const char *cut = "";
    if (bl > REVIEW_BODY_MAX) {
        take = REVIEW_BODY_MAX;
        cut = "\n[正文已截断]";
    }
    snprintf(buf, sz,
             "gate_reason: %s\ndrift: %d\nnode_goal: %s\noutput_body: %.*s%s\n"
             "output_json: %s",
             (gate && gate[0]) ? gate : "gate ok", drift,
             (goal && goal[0]) ? goal : "(无)", take, b, cut,
             (json && json[0]) ? json : "(无)");
    return 0;
}
