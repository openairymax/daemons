// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "gateway_jsonpick.h"

#include "airy_memory.h"

#include <stdarg.h>
#include <stdio.h>

cJSON *gw_json_load(const char *json)
{
    if (!json)
        return NULL;
    return cJSON_Parse(json);
}

char *gw_json_str(const cJSON *root, const char *field)
{
    const cJSON *item = cJSON_GetObjectItem(root, field);
    if (!cJSON_IsString(item) || !item->valuestring)
        return NULL;
    return AIRY_STRDUP(item->valuestring);
}

char *gw_json_raw(const cJSON *root, const char *field)
{
    const cJSON *item = cJSON_GetObjectItem(root, field);
    if (!cJSON_IsString(item) && !cJSON_IsNumber(item) && !cJSON_IsObject(item) &&
        !cJSON_IsArray(item))
        return NULL;
    char *text = cJSON_PrintUnformatted(item);
    if (!text)
        return NULL;
    char *out = AIRY_STRDUP(text);
    cJSON_free(text);
    return out;
}

double gw_json_num(const cJSON *root, const char *field, double def)
{
    const cJSON *item = cJSON_GetObjectItem(root, field);
    return cJSON_IsNumber(item) ? item->valuedouble : def;
}

int gw_json_int(const cJSON *root, const char *field, int def)
{
    const cJSON *item = cJSON_GetObjectItem(root, field);
    return cJSON_IsNumber(item) ? item->valueint : def;
}

char *gw_fmt_alloc(const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0) {
        va_end(ap2);
        return NULL;
    }
    char *buf = (char *)AIRY_MALLOC((size_t)len + 1);
    if (buf)
        vsnprintf(buf, (size_t)len + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}
