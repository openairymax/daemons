// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-C
/**
 * @file daemon_dep.c
 * @brief Daemon hard-dependency declaration and startup probe (governance face).
 *
 * 见 daemon_dep.h 的模块说明：依赖以数据声明，启动期与健康面均经 SD 实时
 * 探测可达性，required 缺失时降级态显式上报。
 */

#include "daemon_dep.h"

#include "airy_memory.h"
#include "hall_writer.h"
#include "service_discovery_helper.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <string.h>

/* 探测只关心可达性，一个实例槽即可；槽数组仅作 sd_helper_find 的输出缓冲，
 * 避免在栈上放置完整的 SD_MAX_INSTANCES 数组。 */
#define DEP_FIND_SLOTS 1

int daemon_dep_init(daemon_dep_t *dep, const daemon_dep_spec_t *specs, size_t count)
{
    if (!dep || (count > 0 && !specs))
        return AIRY_ERR_INVALID_PARAM;

    AIRY_MEMSET(dep, 0, sizeof(*dep));
    if (count > DAEMON_DEP_MAX)
        return AIRY_ERR_BUFFER_TOO_SMALL;

    for (size_t i = 0; i < count; i++)
        dep->specs[i] = specs[i];
    dep->count = count;
    return AIRY_SUCCESS;
}

static bool dep_reachable(sd_helper_t *sdh, const char *name)
{
    sd_instance_t inst[DEP_FIND_SLOTS];
    uint32_t found = 0;

    if (!sdh || !name || !name[0])
        return false;
    if (sd_helper_find(sdh, name, inst, DEP_FIND_SLOTS, &found) != 0)
        return false;
    return found > 0;
}

int daemon_dep_probe(daemon_dep_t *dep, sd_helper_t *sdh)
{
    if (!dep)
        return AIRY_ERR_INVALID_PARAM;

    for (size_t i = 0; i < dep->count; i++)
        dep->reachable[i] = dep_reachable(sdh, dep->specs[i].name);
    dep->probed = true;
    return AIRY_SUCCESS;
}

bool daemon_dep_ready(const daemon_dep_t *dep)
{
    if (!dep || !dep->probed)
        return false;
    return daemon_dep_missing(dep) == 0;
}

size_t daemon_dep_missing(const daemon_dep_t *dep)
{
    size_t missing = 0;

    if (!dep)
        return 0;
    for (size_t i = 0; i < dep->count; i++) {
        if (dep->specs[i].required && !dep->reachable[i])
            missing++;
    }
    return missing;
}

size_t daemon_dep_count(const daemon_dep_t *dep)
{
    return dep ? dep->count : 0;
}

int daemon_dep_at(const daemon_dep_t *dep, size_t idx, const char **name, bool *required,
                  bool *reachable)
{
    if (!dep || idx >= dep->count)
        return AIRY_ERR_INVALID_PARAM;

    if (name)
        *name = dep->specs[idx].name;
    if (required)
        *required = dep->specs[idx].required;
    if (reachable)
        *reachable = dep->reachable[idx];
    return AIRY_SUCCESS;
}

int daemon_dep_note(const daemon_dep_t *dep, char *buf, size_t cap)
{
    size_t used = 0;

    if (!buf || cap == 0)
        return -1;
    buf[0] = '\0';
    if (!dep || !dep->probed)
        return 0;

    for (size_t i = 0; i < dep->count; i++) {
        const char *name;
        size_t name_len;
        size_t need;

        if (!dep->specs[i].required || dep->reachable[i])
            continue;

        name = dep->specs[i].name ? dep->specs[i].name : "?";
        name_len = strlen(name);
        need = used + (used ? 1 : 0) + name_len;
        if (need + 1 > cap)
            break;

        if (used)
            buf[used++] = ',';
        AIRY_MEMCPY(buf + used, name, name_len);
        used += name_len;
        buf[used] = '\0';
    }
    return (int)used;
}

int daemon_dep_report(const daemon_dep_t *dep, const char *daemon_name)
{
    char note[DAEMON_DEP_MAX * (SD_MAX_NAME_LEN + 1) + 1];
    cJSON *evt;
    char *json;
    size_t missing;

    if (!dep || !daemon_name || !daemon_name[0] || !dep->probed)
        return AIRY_ERR_INVALID_PARAM;

    missing = daemon_dep_missing(dep);
    if (missing == 0)
        return 0;

    if (daemon_dep_note(dep, note, sizeof(note)) < 0)
        note[0] = '\0';

    SVC_LOG_WARN("%s: degraded - missing required dependencies: %s", daemon_name, note);

    evt = cJSON_CreateObject();
    if (!evt)
        return (int)missing;

    cJSON_AddStringToObject(evt, "event", "dependency_degraded");
    cJSON_AddStringToObject(evt, "daemon", daemon_name);
    cJSON_AddStringToObject(evt, "missing", note);
    cJSON_AddNumberToObject(evt, "missing_count", (double)missing);

    json = cJSON_PrintUnformatted(evt);
    if (json) {
        (void)daemon_hall_write(daemon_name, "issue", NULL, json);
        cJSON_free(json);
    }
    cJSON_Delete(evt);
    return (int)missing;
}
