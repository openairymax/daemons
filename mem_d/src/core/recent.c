// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file recent.c
 * @brief 最近记忆列表与会话归属过滤。
 *
 * 记忆链展示（CLI --continue、TUI 会话恢复、Web 记忆面板）的权威视图来自
 * mem.recent，其顺序必须与写入序一致，并可按 metadata.conv 隔离会话
 * （0.1.21 S-P1-16）。本文件从 service.c 拆出：该文件已超单文件 SLOC
 * 上限，且 recent 与检索共用同一过滤谓词 mem_rec_in_conv。
 *
 * 会话过滤必须全量倒序扫描后再截断——先取最新 limit 条再过滤会让旧会话
 * 占满窗口，本会话反而为空。
 */

#include "service.h"
#include "airy_types.h"

#include <cjson/cJSON.h>
#include <string.h>

#define MEM_RECENT_DEFAULT_LIMIT 10

/* metadata.conv 与给定值相等即命中；无 metadata / 无 conv 视为不匹配。 */
int mem_rec_in_conv(const mem_record_entry_t *rec, const char *conv)
{
    if (!rec || !rec->metadata || !conv)
        return 0;
    cJSON *root = cJSON_Parse(rec->metadata);
    if (!root)
        return 0;
    cJSON *c = cJSON_GetObjectItem(root, "conv");
    int hit = cJSON_IsString(c) && c->valuestring && strcmp(c->valuestring, conv) == 0;
    cJSON_Delete(root);
    return hit;
}

int mem_service_recent_scoped(mem_service_t *svc, uint32_t limit, const char *conv,
                              mem_recent_item_t **out_items, size_t *out_count)
{
    if (!svc || !svc->initialized || !out_items || !out_count)
        return AIRY_ERR_INVALID_PARAM;
    *out_items = NULL;
    *out_count = 0;

    if (limit == 0)
        limit = MEM_RECENT_DEFAULT_LIMIT;

    airy_mtx_lock(&svc->lock);

    size_t total = svc->record_count;
    size_t take = total < limit ? total : limit;
    mem_recent_item_t *items =
        take ? (mem_recent_item_t *)AIRY_CALLOC(take, sizeof(mem_recent_item_t)) : NULL;
    if (take && !items) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 下标倒序即写入序倒序：写入总落在 record_count 处，删除以 memmove
     * 连续压实（mem_remove_record_at），故数组下标恒等于写入序。不依赖
     * created_at（秒级精度，同秒多写时会并列）。 */
    size_t n = 0;
    for (size_t i = 0; i < total && n < take; i++) {
        const mem_record_entry_t *rec = &svc->records[total - 1 - i];
        if (!rec->record_id || !rec->data)
            continue;
        if (conv && !mem_rec_in_conv(rec, conv))
            continue;
        mem_recent_item_t *it = &items[n];
        it->record_id = AIRY_STRDUP(rec->record_id);
        if (!mem_rec_copy_out(rec, &it->data, &it->len, &it->metadata)) {
            AIRY_FREE(it->record_id);
            it->record_id = NULL;
            continue;
        }
        it->created_at = rec->created_at;
        n++;
    }

    airy_mtx_unlock(&svc->lock);

    if (n == 0) {
        AIRY_FREE(items);
        return AIRY_SUCCESS;
    }

    mem_recent_item_t *shrunk = (mem_recent_item_t *)AIRY_REALLOC(items, n * sizeof(*items));
    *out_items = shrunk ? shrunk : items;
    *out_count = n;
    return AIRY_SUCCESS;
}

int mem_service_recent(mem_service_t *svc, uint32_t limit,
                       mem_recent_item_t **out_items, size_t *out_count)
{
    return mem_service_recent_scoped(svc, limit, NULL, out_items, out_count);
}

void mem_recent_items_free(mem_recent_item_t *items, size_t count)
{
    if (!items)
        return;
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(items[i].record_id);
        AIRY_FREE(items[i].data);
        AIRY_FREE(items[i].metadata);
    }
    AIRY_FREE(items);
}
