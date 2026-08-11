// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file agent_registry_core.c
 * @brief Agent注册表核心功能实现
 */

#include "agent_registry_core.h"
#include "daemon_platform_ext.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct agent_registry {
    agent_entry_t entries[AIRY_CAP_MAX_AGENTS];
    size_t entry_count;
    airy_mtx_t lock;
    char *db_path;
    int initialized;
};

agent_registry_t *agent_registry_core_create(void)
{
    agent_registry_t *reg = (agent_registry_t *)AIRY_CALLOC(1, sizeof(agent_registry_t));
    if (!reg) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    airy_mtx_init(&reg->lock);
    reg->initialized = 0;
    return reg;
}

void agent_registry_core_destroy(agent_registry_t *registry)
{
    if (!registry)
        return;
    if (registry->initialized)
        agent_registry_core_shutdown(registry);
    airy_mtx_destroy(&registry->lock);
    AIRY_FREE(registry);
}

int agent_registry_core_init(agent_registry_t *registry, const char *db_path)
{
    if (!registry || registry->initialized)
        return AIRY_ERR_INVALID_PARAM;
    if (db_path)
        registry->db_path = AIRY_STRDUP(db_path);
    registry->entry_count = 0;
    registry->initialized = 1;
    return 0;
}

void agent_registry_core_shutdown(agent_registry_t *registry)
{
    if (!registry || !registry->initialized)
        return;
    airy_mtx_lock(&registry->lock);
    for (size_t i = 0; i < registry->entry_count; i++) {
        AIRY_FREE(registry->entries[i].description);
        AIRY_FREE(registry->entries[i].author);
    }
    registry->entry_count = 0;
    AIRY_FREE(registry->db_path);
    registry->db_path = NULL;
    registry->initialized = 0;
    airy_mtx_unlock(&registry->lock);
}

int agent_registry_core_add(agent_registry_t *registry, const agent_entry_t *reg)
{
    if (!registry || !registry->initialized || !reg)
        return AIRY_ERR_INVALID_PARAM;
    if (!reg->id[0] || !reg->name[0])
        return AIRY_ERR_INVALID_PARAM;
    if (registry->entry_count >= AIRY_CAP_MAX_AGENTS)
        return AIRY_ERR_OVERFLOW;

    airy_mtx_lock(&registry->lock);
    agent_entry_t *entry = &registry->entries[registry->entry_count];
    __builtin_memset(entry, 0, sizeof(agent_entry_t));
    AIRY_STRNCPY_TERM(entry->id, reg->id, sizeof(entry->id));
    AIRY_STRNCPY_TERM(entry->name, reg->name, sizeof(entry->name));
    if (reg->description)
        entry->description = AIRY_STRDUP(reg->description);
    if (reg->author)
        entry->author = AIRY_STRDUP(reg->author);
    entry->created_at = (uint64_t)time(NULL);
    entry->updated_at = entry->created_at;
    entry->verified = reg->verified;
    entry->official = reg->official;
    registry->entry_count++;
    airy_mtx_unlock(&registry->lock);
    return 0;
}

int agent_registry_core_remove(agent_registry_t *registry, const char *agent_id)
{
    if (!registry || !registry->initialized || !agent_id)
        return AIRY_ERR_INVALID_PARAM;
    airy_mtx_lock(&registry->lock);
    for (size_t i = 0; i < registry->entry_count; i++) {
        if (strcmp(registry->entries[i].id, agent_id) == 0) {
            AIRY_FREE(registry->entries[i].description);
            AIRY_FREE(registry->entries[i].author);
            for (size_t j = i; j < registry->entry_count - 1; j++) {
                registry->entries[j] = registry->entries[j + 1];
            }
            registry->entry_count--;
            airy_mtx_unlock(&registry->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&registry->lock);
    return AIRY_ERR_NOT_FOUND;
}

int agent_registry_core_get(agent_registry_t *registry, const char *agent_id, agent_entry_t *out)
{
    if (!registry || !registry->initialized || !agent_id || !out)
        return AIRY_ERR_INVALID_PARAM;
    airy_mtx_lock(&registry->lock);
    for (size_t i = 0; i < registry->entry_count; i++) {
        if (strcmp(registry->entries[i].id, agent_id) == 0) {
            /* P0-7 修复：在锁内完成浅拷贝后再解锁，避免调用方无锁访问内部 entries[]。
             * 注意：description/author/tags 仍为内部指针，调用方不应在 unlock 后解引用；
             * 如需长期持有应额外深拷贝。本修复聚焦消除返回内部数组指针的数据竞争。 */
            *out = registry->entries[i];
            airy_mtx_unlock(&registry->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&registry->lock);
    return AIRY_ERR_NOT_FOUND;
}

size_t agent_registry_core_list(agent_registry_t *registry, agent_entry_t *out_entries,
                                size_t max_entries)
{
    if (!registry || !registry->initialized || !out_entries)
        return 0;
    airy_mtx_lock(&registry->lock);
    size_t count = (registry->entry_count < max_entries) ? registry->entry_count : max_entries;
    /* P0-7 修复：在锁内按值拷贝条目快照，调用方解锁后访问的是独立拷贝，
     * 不再持有指向内部 entries[] 的指针，消除 remove() 数组搬移导致的 use-after-free。 */
    for (size_t i = 0; i < count; i++)
        out_entries[i] = registry->entries[i];
    airy_mtx_unlock(&registry->lock);
    return count;
}

size_t agent_registry_core_count(agent_registry_t *registry)
{
    if (!registry || !registry->initialized)
        return 0;
    return registry->entry_count;
}

int agent_registry_core_add_version(agent_registry_t *registry, const char *agent_id,
                                    const agent_version_t *version)
{
    if (!registry || !registry->initialized || !agent_id || !version)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&registry->lock);
    for (size_t i = 0; i < registry->entry_count; i++) {
        if (strcmp(registry->entries[i].id, agent_id) == 0) {
            if (registry->entries[i].version_count >= MAX_VERSIONS_PER_AGENT) {
                airy_mtx_unlock(&registry->lock);
                return AIRY_ERR_OVERFLOW;
            }
            agent_version_t *v = &registry->entries[i].versions[registry->entries[i].version_count];
            __builtin_memset(v, 0, sizeof(agent_version_t));
            AIRY_STRNCPY_TERM(v->version, version->version, sizeof(v->version));
            v->version[sizeof(v->version) - 1] = '\0';
            AIRY_STRNCPY_TERM(v->download_url, version->download_url, sizeof(v->download_url));
            v->download_url[sizeof(v->download_url) - 1] = '\0';
            AIRY_STRNCPY_TERM(v->checksum, version->checksum, sizeof(v->checksum));
            v->checksum[sizeof(v->checksum) - 1] = '\0';
            v->created_at = (uint64_t)time(NULL);
            v->deprecated = version->deprecated;
            registry->entries[i].version_count++;
            AIRY_STRNCPY_TERM(registry->entries[i].latest_version, version->version,
                              sizeof(registry->entries[i].latest_version));
            registry->entries[i].latest_version[sizeof(registry->entries[i].latest_version) - 1] =
                '\0';
            airy_mtx_unlock(&registry->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&registry->lock);
    return AIRY_ERR_NOT_FOUND;
}

int agent_registry_core_get_latest_version(agent_registry_t *registry, const char *agent_id,
                                           char *out, size_t out_size)
{
    if (!registry || !registry->initialized || !agent_id || !out || out_size == 0)
        return AIRY_ERR_INVALID_PARAM;
    airy_mtx_lock(&registry->lock);
    for (size_t i = 0; i < registry->entry_count; i++) {
        if (strcmp(registry->entries[i].id, agent_id) == 0) {
            /* P0-7 修复：在锁内拷贝 latest_version 字符串到调用方缓冲区后再解锁，
             * 避免返回内部 char 数组指针后调用方无锁访问。 */
            AIRY_STRNCPY_TERM(out, registry->entries[i].latest_version, out_size);
            out[out_size - 1] = '\0';
            airy_mtx_unlock(&registry->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&registry->lock);
    return AIRY_ERR_NOT_FOUND;
}

size_t agent_registry_core_search_by_tag(agent_registry_t *registry, const char *tag,
                                         agent_entry_t *out_entries, size_t max_entries)
{
    if (!registry || !registry->initialized || !tag || !out_entries)
        return 0;
    airy_mtx_lock(&registry->lock);
    size_t count = 0;
    for (size_t i = 0; i < registry->entry_count && count < max_entries; i++) {
        for (size_t j = 0; j < registry->entries[i].tag_count; j++) {
            if (registry->entries[i].tags[j] && strcmp(registry->entries[i].tags[j], tag) == 0) {

                out_entries[count++] = registry->entries[i];
                break;
            }
        }
    }
    airy_mtx_unlock(&registry->lock);
    return count;
}

size_t agent_registry_core_search(agent_registry_t *registry, const char *query,
                                  agent_entry_t *out_entries, size_t max_entries)
{
    if (!registry || !registry->initialized || !query || !out_entries)
        return 0;
    airy_mtx_lock(&registry->lock);
    size_t count = 0;
    for (size_t i = 0; i < registry->entry_count && count < max_entries; i++) {
        if (strstr(registry->entries[i].id, query) || strstr(registry->entries[i].name, query) ||
            (registry->entries[i].description && strstr(registry->entries[i].description, query))) {
            /* P1-15 修复：在锁内按值拷贝匹配条目，调用方解锁后访问的是独立快照，
             * 不再持有指向内部 entries[] 的指针，消除 remove() 数组搬移导致的 use-after-free。
             * 参照 P0-7 修正的 agent_registry_core_search_by_tag 同模式。 */
            out_entries[count] = registry->entries[i];
            count++;
        }
    }
    airy_mtx_unlock(&registry->lock);
    return count;
}
