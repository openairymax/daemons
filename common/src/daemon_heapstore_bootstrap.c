// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_heapstore_bootstrap.c
 * @brief daemon 统一 heapstore 运行时数据存储引导实现
 */

#include "daemon_heapstore_bootstrap.h"

#include "platform.h"
#include "heapstore_integration.h"
#include "svc_logger.h"

static int g_heapstore_initialized = 0;

airy_err_t daemon_heapstore_init(const char *daemon_name)
{
    if (!daemon_name) {
        SVC_LOG_ERROR("daemon_heapstore_init: NULL daemon_name");
        return AIRY_EINVAL;
    }

    if (g_heapstore_initialized) {
        SVC_LOG_DEBUG("daemon_heapstore_init: heapstore already initialized (daemon=%s)",
                      daemon_name);
        return AIRY_SUCCESS;
    }

    airy_paths_init();

    char root_path[512];
    const char *data_dir = airy_data_dir();
    if (data_dir && data_dir[0]) {
        snprintf(root_path, sizeof(root_path), "%s/agentrt/heapstore", data_dir);
    } else {

        root_path[0] = '\0';
    }

    airy_err_t err = heapstore_integration_init(root_path[0] ? root_path : NULL);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("daemon_heapstore_init: heapstore_integration_init FAILED for "
                      "daemon='%s' (err=%d, root=%s) — runtime data store unavailable, "
                      "service layer will degrade (non-fatal)",
                      daemon_name, (int)err, root_path[0] ? root_path : "(default)");
        return err;
    }

    g_heapstore_initialized = 1;
    SVC_LOG_INFO("daemon_heapstore_init: heapstore runtime data store initialized for "
                 "'%s' (root=%s)",
                 daemon_name, root_path[0] ? root_path : "(default)");
    return AIRY_SUCCESS;
}

void daemon_heapstore_cleanup(void)
{
    if (!g_heapstore_initialized)
        return;

    heapstore_integration_shutdown();
    g_heapstore_initialized = 0;
    SVC_LOG_INFO("daemon_heapstore_cleanup: heapstore runtime data store shut down");
}

int daemon_heapstore_log(const char *module, int level, const char *msg, const char *trace_id)
{
    if (!g_heapstore_initialized || !module || !msg)
        return -1;

    airy_err_t err = heapstore_logging_write(module, level, trace_id, msg, airy_time_ns());
    return (err == AIRY_SUCCESS) ? 0 : -1;
}
