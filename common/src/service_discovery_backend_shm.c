// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_discovery_backend_shm.c
 * @brief Cross-process service-discovery shm backend: in-memory registry
 *        (default, cross-process shared-memory semantics).
 */

#include "service_discovery_internal.h"

static airy_err_t shm_init(sd_internal_t *sd)
{
    (void)sd;
    return AIRY_SUCCESS;
}

static void shm_deinit(sd_internal_t *sd)
{
    (void)sd;
}

static airy_err_t shm_register(sd_internal_t *sd, const char *name, const char *type,
                               const sd_instance_t *inst, const char *tags, const char *deps)
{
    return sd_registry_add_instance(sd, name, type, inst, tags, deps);
}

static airy_err_t shm_deregister(sd_internal_t *sd, const char *name, const char *instance_id)
{
    return sd_registry_remove_instance(sd, name, instance_id, NULL);
}

static airy_err_t shm_deregister_all(sd_internal_t *sd, const char *name)
{
    int32_t svc_idx = find_service_index(sd, name);
    if (svc_idx < 0)
        return AIRY_ENOENT;
    sd->services[svc_idx].instance_count = 0;
    sd->services[svc_idx].last_updated = airy_time_ms();
    return AIRY_SUCCESS;
}

static airy_err_t shm_lookup(sd_internal_t *sd, const char *name, sd_service_entry_t *out)
{
    int32_t svc_idx = find_service_index(sd, name);
    if (svc_idx < 0)
        return AIRY_ENOENT;
    __builtin_memcpy(out, &sd->services[svc_idx], sizeof(sd_service_entry_t));
    return AIRY_SUCCESS;
}

static airy_err_t shm_list(sd_internal_t *sd, sd_service_entry_t *out, uint32_t max,
                           uint32_t *count)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < sd->service_count && n < max; i++)
        __builtin_memcpy(&out[n++], &sd->services[i], sizeof(sd_service_entry_t));
    *count = n;
    return AIRY_SUCCESS;
}

static airy_err_t shm_refresh(sd_internal_t *sd, const char *name)
{
    (void)sd;
    (void)name;
    return AIRY_SUCCESS;
}

static airy_err_t shm_commit(sd_internal_t *sd, const char *name)
{
    (void)sd;
    (void)name;
    return AIRY_SUCCESS;
}

const sd_backend_t sd_backend_shm = {
    .name = "shm",
    .init = shm_init,
    .deinit = shm_deinit,
    .register_service = shm_register,
    .deregister_service = shm_deregister,
    .deregister_all = shm_deregister_all,
    .lookup_service = shm_lookup,
    .list_services = shm_list,
    .refresh = shm_refresh,
    .commit = shm_commit,
};
