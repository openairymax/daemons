/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_registry_core.h
 * @brief Agent注册表核心功能接口
 */

#ifndef AIRY_RT_AGENT_REGISTRY_CORE_H
#define AIRY_RT_AGENT_REGISTRY_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <airymax/sched.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_AGENT_ID_LEN 128
#define MAX_AGENT_NAME_LEN 256
#define MAX_DESCRIPTION_LEN 4096
#define MAX_TAGS_PER_AGENT 16
#define MAX_VERSIONS_PER_AGENT 32
#define MAX_URL_LEN 512
#define MAX_CHECKSUM_LEN 128

typedef struct agent_version {
    char version[32];
    char download_url[MAX_URL_LEN];
    char checksum[MAX_CHECKSUM_LEN];
    uint64_t created_at;
    int deprecated;
} agent_version_t;

typedef struct agent_entry {
    char id[MAX_AGENT_ID_LEN];
    char name[MAX_AGENT_NAME_LEN];
    char *description;
    char *author;
    char *tags[MAX_TAGS_PER_AGENT];
    size_t tag_count;
    agent_version_t versions[MAX_VERSIONS_PER_AGENT];
    size_t version_count;
    char latest_version[32];
    uint64_t created_at;
    uint64_t updated_at;
    int verified;
    int official;
} agent_entry_t;

typedef struct agent_registry agent_registry_t;

agent_registry_t *agent_registry_core_create(void);
void agent_registry_core_destroy(agent_registry_t *registry);
int agent_registry_core_init(agent_registry_t *registry, const char *db_path);
void agent_registry_core_shutdown(agent_registry_t *registry);

int agent_registry_core_add(agent_registry_t *registry, const agent_entry_t *reg);
int agent_registry_core_remove(agent_registry_t *registry, const char *agent_id);

/**
 * @brief 按 agent_id 查询并拷贝条目到调用方缓冲区。
 * @param registry 注册表句柄。
 * @param agent_id 目标 agent ID。
 * @param out 调用方提供的缓冲区，命中时填充条目快照（浅拷贝）。
 * @return 0 成功；AIRY_ERR_INVALID_PARAM 参数非法；AIRY_ERR_NOT_FOUND 未命中。
 * @note 数据在锁内拷贝完成后解锁，避免调用方无锁访问内部 entries[]。
 */
int agent_registry_core_get(agent_registry_t *registry, const char *agent_id, agent_entry_t *out);

/**
 * @brief 列出全部条目并拷贝到调用方缓冲区。
 * @param registry 注册表句柄。
 * @param out_entries 调用方提供的数组缓冲区（按值拷贝，非指针）。
 * @param max_entries 缓冲区最大容量。
 * @return 实际拷贝的条目数。
 * @note 拷贝在锁内完成，调用方解锁后访问的是独立快照。
 */
size_t agent_registry_core_list(agent_registry_t *registry, agent_entry_t *out_entries,
                                size_t max_entries);
size_t agent_registry_core_count(agent_registry_t *registry);

int agent_registry_core_add_version(agent_registry_t *registry, const char *agent_id,
                                    const agent_version_t *version);

/**
 * @brief 查询指定 agent 的最新版本号并拷贝到调用方缓冲区。
 * @param registry 注册表句柄。
 * @param agent_id 目标 agent ID。
 * @param out 调用方提供的字符串缓冲区。
 * @param out_size 缓冲区字节数（含结尾 '\0'）。
 * @return 0 成功；AIRY_ERR_INVALID_PARAM 参数非法；AIRY_ERR_NOT_FOUND 未命中。
 * @note 字符串拷贝在锁内完成，避免返回内部指针后无锁访问。
 */
int agent_registry_core_get_latest_version(agent_registry_t *registry, const char *agent_id,
                                           char *out, size_t out_size);

/**
 * @brief 按 tag 检索并拷贝匹配条目到调用方缓冲区。
 * @param registry 注册表句柄。
 * @param tag 目标标签。
 * @param out_entries 调用方提供的数组缓冲区（按值拷贝，非指针）。
 * @param max_entries 缓冲区最大容量。
 * @return 实际拷贝的匹配条目数。
 * @note 拷贝在锁内完成，调用方解锁后访问的是独立快照。
 */
size_t agent_registry_core_search_by_tag(agent_registry_t *registry, const char *tag,
                                         agent_entry_t *out_entries, size_t max_entries);

/**
 * @brief 按关键词模糊检索并拷贝匹配条目到调用方缓冲区。
 * @param registry 注册表句柄。
 * @param query 查询关键词（匹配 id/name/description 子串）。
 * @param out_entries 调用方提供的数组缓冲区（按值拷贝，非指针）。
 * @param max_entries 缓冲区最大容量。
 * @return 实际拷贝的匹配条目数。
 * @note P1-15 修复：拷贝在锁内完成，调用方解锁后访问的是独立快照，
 *       不再返回指向内部 entries[] 的指针，消除数据竞争。
 */
size_t agent_registry_core_search(agent_registry_t *registry, const char *query,
                                  agent_entry_t *out_entries, size_t max_entries);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_AGENT_REGISTRY_CORE_H */
