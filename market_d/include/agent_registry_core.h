/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_registry_core.h
 * @brief Core agent-registry functionality interface.
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
 * @brief Look up an entry by agent_id and copy it into the caller's buffer.
 * @param registry Registry handle.
 * @param agent_id Target agent ID.
 * @param out Caller-provided buffer; filled with an entry snapshot (shallow copy) on hit.
 * @return 0 on success; AIRY_ERR_INVALID_PARAM bad args; AIRY_ERR_NOT_FOUND no hit.
 * @note Data is copied under the lock and the lock released afterwards, so the
 *       caller never touches internal entries[] unlocked.
 */
int agent_registry_core_get(agent_registry_t *registry, const char *agent_id, agent_entry_t *out);

/**
 * @brief List all entries and copy them into the caller's buffer.
 * @param registry Registry handle.
 * @param out_entries Caller-provided array buffer (copied by value, not pointers).
 * @param max_entries Buffer capacity.
 * @return Number of entries actually copied.
 * @note Copying happens under the lock; after unlocking, the caller accesses
 *       an independent snapshot.
 */
size_t agent_registry_core_list(agent_registry_t *registry, agent_entry_t *out_entries,
                                size_t max_entries);
size_t agent_registry_core_count(agent_registry_t *registry);

int agent_registry_core_add_version(agent_registry_t *registry, const char *agent_id,
                                    const agent_version_t *version);

/**
 * @brief Query the latest version of the given agent and copy it into the
 *        caller's buffer.
 * @param registry Registry handle.
 * @param agent_id Target agent ID.
 * @param out Caller-provided string buffer.
 * @param out_size Buffer size in bytes (including trailing '\0').
 * @return 0 on success; AIRY_ERR_INVALID_PARAM bad args; AIRY_ERR_NOT_FOUND no hit.
 * @note The string is copied under the lock, avoiding unlocked access to an
 *       internal pointer.
 */
int agent_registry_core_get_latest_version(agent_registry_t *registry, const char *agent_id,
                                           char *out, size_t out_size);

/**
 * @brief Search by tag and copy matching entries into the caller's buffer.
 * @param registry Registry handle.
 * @param tag Target tag.
 * @param out_entries Caller-provided array buffer (copied by value, not pointers).
 * @param max_entries Buffer capacity.
 * @return Number of matching entries actually copied.
 * @note Copying happens under the lock; after unlocking, the caller accesses
 *       an independent snapshot.
 */
size_t agent_registry_core_search_by_tag(agent_registry_t *registry, const char *tag,
                                         agent_entry_t *out_entries, size_t max_entries);

/**
 * @brief Fuzzy search by keyword and copy matching entries into the caller's
 *        buffer.
 * @param registry Registry handle.
 * @param query Query keyword (matches id/name/description substrings).
 * @param out_entries Caller-provided array buffer (copied by value, not pointers).
 * @param max_entries Buffer capacity.
 * @return Number of matching entries actually copied.
 * @note P1-15 fix: copying happens under the lock; after unlocking, the
 *       caller accesses an independent snapshot, no longer returning pointers
 *       into the internal entries[], eliminating the data race.
 */
size_t agent_registry_core_search(agent_registry_t *registry, const char *query,
                                  agent_entry_t *out_entries, size_t max_entries);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_AGENT_REGISTRY_CORE_H */
