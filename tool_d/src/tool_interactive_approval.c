// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tool_interactive_approval.c
 * @brief P0：工具级交互式权限审批实现
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"
#include "sync.h"
#include "tool_interactive_approval.h"

#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>

#define AIRY_APPROVAL_DEFAULT_TIMEOUT_MS 120000u

#define ENV_APPROVAL_MODE "AIRY_TOOL_APPROVAL_MODE"
#define ENV_APPROVAL_TIMEOUT "AIRY_TOOL_APPROVAL_TIMEOUT_MS"
#define ENV_MODE_INTERACTIVE "interactive"

typedef struct interactive_pending_req {
    char *request_id;
    char *tool;
    char *agent_id;
    char *params;
    uint64_t created_at;
    airy_approval_outcome_t outcome;
    int resolved;
    struct interactive_pending_req *next;
} interactive_pending_req_t;

struct interactive_approval {
    bool enabled;
    uint64_t timeout_ms;
    sync_mutex_t lock;
    sync_condition_t cond;
    interactive_pending_req_t *head;
    uint64_t seq;
};

static void pending_req_free(interactive_pending_req_t *req)
{
    if (!req)
        return;
    AIRY_FREE(req->request_id);
    AIRY_FREE(req->tool);
    AIRY_FREE(req->agent_id);
    AIRY_FREE(req->params);
    AIRY_FREE(req);
}

interactive_approval_t *interactive_approval_create(void)
{
    interactive_approval_t *mgr =
        (interactive_approval_t *)AIRY_CALLOC(1, sizeof(interactive_approval_t));
    if (!mgr) {
        SVC_LOG_ERROR("interactive_approval_create: calloc failed");
        return NULL;
    }

    mgr->timeout_ms = AIRY_APPROVAL_DEFAULT_TIMEOUT_MS;

    const char *mode = getenv(ENV_APPROVAL_MODE);
    mgr->enabled = (mode && strcmp(mode, ENV_MODE_INTERACTIVE) == 0);

    const char *timeout_str = getenv(ENV_APPROVAL_TIMEOUT);
    if (timeout_str) {
        long v = strtol(timeout_str, NULL, 10);
        if (v > 0) {
            mgr->timeout_ms = (uint64_t)v;
        }
    }

    mgr->seq = 0;
    mgr->head = NULL;

    if (sync_mutex_create(&mgr->lock, NULL) != SYNC_SUCCESS) {
        SVC_LOG_ERROR("interactive_approval_create: mutex create failed");
        AIRY_FREE(mgr);
        return NULL;
    }
    if (sync_condition_create(&mgr->cond, NULL) != SYNC_SUCCESS) {
        SVC_LOG_ERROR("interactive_approval_create: condition create failed");
        sync_mutex_free(mgr->lock);
        AIRY_FREE(mgr);
        return NULL;
    }

    SVC_LOG_INFO("P0: Interactive tool approval %s (timeout_ms=%llu)",
                 mgr->enabled ? "ENABLED" : "disabled", (unsigned long long)mgr->timeout_ms);
    return mgr;
}

void interactive_approval_destroy(interactive_approval_t *mgr)
{
    if (!mgr)
        return;

    /* 唤醒仍在阻塞等待的线程，避免销毁时挂起。
     * 被唤醒的 wait 线程会因 req 未 resolved 而按超时（DENIED）处理。 */
    sync_condition_broadcast_ex(mgr->cond);

    sync_mutex_lock_ex(mgr->lock, NULL);
    interactive_pending_req_t *p = mgr->head;
    while (p) {
        interactive_pending_req_t *next = p->next;
        pending_req_free(p);
        p = next;
    }
    mgr->head = NULL;
    sync_mutex_unlock_ex(mgr->lock);

    sync_condition_free(mgr->cond);
    sync_mutex_free(mgr->lock);
    AIRY_FREE(mgr);
}

bool interactive_approval_is_enabled(const interactive_approval_t *mgr)
{
    return (mgr != NULL) && mgr->enabled;
}

char *interactive_approval_block(interactive_approval_t *mgr, const char *tool,
                                 const char *agent_id, const char *params_json,
                                 airy_approval_outcome_t *out_outcome)
{
    if (!mgr || !tool || !out_outcome) {
        return NULL;
    }
    if (!mgr->enabled) {
        return NULL;
    }

    interactive_pending_req_t *req =
        (interactive_pending_req_t *)AIRY_CALLOC(1, sizeof(interactive_pending_req_t));
    if (!req) {
        SVC_LOG_ERROR("interactive_approval_block: calloc failed");
        return NULL;
    }

    char reqbuf[72];
    sync_mutex_lock_ex(mgr->lock, NULL);
    uint64_t now = sync_get_timestamp_ms();
    snprintf(reqbuf, sizeof(reqbuf), "req_%llu_%llu", (unsigned long long)now,
             (unsigned long long)mgr->seq);
    req->request_id = AIRY_STRDUP(reqbuf);
    req->tool = AIRY_STRDUP(tool);
    req->agent_id = AIRY_STRDUP(agent_id ? agent_id : "unknown");
    req->params = AIRY_STRDUP(params_json ? params_json : "");
    req->created_at = now;
    req->outcome = AIRY_APPROVAL_DENIED;
    req->resolved = 0;
    req->next = NULL;
    mgr->seq++;

    interactive_pending_req_t **pp = &mgr->head;
    while (*pp) {
        pp = &(*pp)->next;
    }
    *pp = req;

    SVC_LOG_INFO("P0: Interactive approval pending req=%s tool='%s' agent='%s'", req->request_id,
                 tool, agent_id ? agent_id : "?");

    uint64_t deadline = now + mgr->timeout_ms;
    while (!req->resolved) {
        uint64_t now_ms = sync_get_timestamp_ms();
        if (now_ms >= deadline) {
            break;
        }
        sync_timeout_t to = {deadline - now_ms, false};
        sync_result_t wr = sync_condition_wait_ex(mgr->cond, mgr->lock, &to);
        if (wr != SYNC_SUCCESS) {
            break;
        }
    }

    interactive_pending_req_t **q = &mgr->head;
    while (*q && *q != req) {
        q = &(*q)->next;
    }
    if (*q) {
        *q = req->next;
    }
    sync_mutex_unlock_ex(mgr->lock);

    *out_outcome = req->outcome;
    char *request_id = AIRY_STRDUP(req->request_id);
    SVC_LOG_INFO("P0: Interactive approval resolved req=%s decision=%d", req->request_id,
                 (int)req->outcome);
    pending_req_free(req);
    return request_id;
}

int interactive_approval_resolve(interactive_approval_t *mgr, const char *request_id,
                                 const char *decision)
{
    if (!mgr || !request_id || !decision) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_approval_outcome_t outcome;
    if (strcmp(decision, "allow") == 0) {
        outcome = AIRY_APPROVAL_ALLOWED;
    } else if (strcmp(decision, "always") == 0) {
        outcome = AIRY_APPROVAL_ALWAYS;
    } else if (strcmp(decision, "deny") == 0) {
        outcome = AIRY_APPROVAL_DENIED;
    } else {
        return AIRY_ERR_INVALID_PARAM;
    }

    sync_mutex_lock_ex(mgr->lock, NULL);
    interactive_pending_req_t *p = mgr->head;
    while (p) {
        if (strcmp(p->request_id, request_id) == 0) {
            if (p->resolved) {

                sync_mutex_unlock_ex(mgr->lock);
                return AIRY_ERR_STATE_ERROR;
            }
            p->resolved = 1;
            p->outcome = outcome;
            sync_mutex_unlock_ex(mgr->lock);
            sync_condition_broadcast_ex(mgr->cond);
            SVC_LOG_INFO("P0: Interactive approval decision req=%s decision=%s", request_id,
                         decision);
            return 0;
        }
        p = p->next;
    }
    sync_mutex_unlock_ex(mgr->lock);
    return AIRY_ERR_NOT_FOUND;
}

char *interactive_approval_pending_list_json(interactive_approval_t *mgr)
{
    if (!mgr) {
        return NULL;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }

    sync_mutex_lock_ex(mgr->lock, NULL);
    interactive_pending_req_t *p = mgr->head;
    while (p) {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddStringToObject(o, "request_id", p->request_id);
            cJSON_AddStringToObject(o, "tool", p->tool);
            cJSON_AddStringToObject(o, "agent_id", p->agent_id);
            cJSON_AddStringToObject(o, "params", p->params);
            cJSON_AddNumberToObject(o, "created_at", (double)(uint64_t)p->created_at);
            cJSON_AddItemToArray(arr, o);
        }
        p = p->next;
    }
    sync_mutex_unlock_ex(mgr->lock);

    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}