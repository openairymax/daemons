#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file service.c
 * @brief Cupolas 安全穹顶服务实现：权限裁决/输入净化/命令执行/规则管理/审计
 *
 * cupolas_d 将 cupolas 安全库（agentrt/cupolas）封装为独立 daemon 服务。
 * 本服务真实调用 cupolas.h 公共 API（IRON-2：所有方法无桩）：
 *   - cupolas_check_permission / cupolas_add_permission_rule
 *   - cupolas_sanitize_input / cupolas_execute_command
 *   - cupolas_flush_audit_log / cupolas_version
 *
 * 设计要点：
 * - cupolas 为进程级单例库（cupolas_init），模块初始化由 main() 通过
 *   daemon_cupolas_init("cupolas_d") 完成；本服务实例仅承载配置元数据与
 *   真实运行统计（原子计数器：权限检查次数 / 净化次数）。
 * - 统计为真实计数：每次 check_permission / sanitize 调用都会递增，
 *   供 cupolas.get_stats 返回。
 */

#include "cupolas_service.h"

#include "cupolas.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* 命令执行输出缓冲区大小（64KB，覆盖常规命令输出） */
#define CUPOLAS_EXEC_OUTPUT_SIZE (64 * 1024)
/* 净化输出缓冲区上限（1MB；daemon 请求缓冲 MAX_BUFFER=64KB，6x 放大足够） */
#define CUPOLAS_SANITIZE_MAX_OUTPUT (1024 * 1024)

/* ==================== 服务实例 ==================== */

struct cupolas_service {
    char *config_path;                  /* cupolas 配置文件路径（可 NULL） */
    int64_t start_time;                 /* 服务启动时间（秒） */
    atomic_uint_fast64_t permission_checks; /* 权限裁决真实计数 */
    atomic_uint_fast64_t sanitize_count;    /* 输入净化真实计数 */
};

/* ==================== 生命周期 ==================== */

cupolas_service_t *cupolas_service_create(const char *config_path)
{
    cupolas_service_t *svc = AIRY_CALLOC(1, sizeof(cupolas_service_t));
    if (!svc)
        return NULL;

    svc->config_path = config_path ? AIRY_STRDUP(config_path) : NULL;
    svc->start_time = (int64_t)time(NULL);
    atomic_init(&svc->permission_checks, 0);
    atomic_init(&svc->sanitize_count, 0);
    return svc;
}

void cupolas_service_destroy(cupolas_service_t *svc)
{
    if (!svc)
        return;
    AIRY_FREE(svc->config_path);
    AIRY_FREE(svc);
}

/* ==================== 权限裁决 ==================== */

int cupolas_service_check_permission(cupolas_service_t *svc,
                                     const cupolas_check_permission_params_t *params,
                                     cupolas_check_permission_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->agent_id || !params->action || !params->resource)
        return AIRY_ERR_INVALID_PARAM;

    /* 真实统计：每次裁决递增（IRON-2，供 get_stats 返回） */
    atomic_fetch_add_explicit(&svc->permission_checks, 1, memory_order_relaxed);

    int ret = cupolas_check_permission(params->agent_id, params->action, params->resource,
                                       params->context);
    if (ret < 0) {
        SVC_LOG_ERROR("cupolas.check_permission failed: agent=%s action=%s resource=%s rc=%d",
                      params->agent_id, params->action, params->resource, ret);
        return ret;
    }

    out->allowed = (ret > 0) ? 1 : 0;
    out->err = ret;
    SVC_LOG_INFO("cupolas.check_permission: agent=%s action=%s resource=%s -> %s",
                 params->agent_id, params->action, params->resource,
                 out->allowed ? "allowed" : "denied");
    return AIRY_SUCCESS;
}

/* ==================== 输入净化 ==================== */

int cupolas_service_sanitize(cupolas_service_t *svc,
                             const cupolas_sanitize_params_t *params,
                             cupolas_sanitize_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->input)
        return AIRY_ERR_INVALID_PARAM;

    size_t in_len = strlen(params->input);
    /* HTML 实体转义最大约 5x 放大，取 6x + 余量保证输出不被截断 */
    size_t out_size = in_len * 6 + 64;
    if (out_size < 256)
        out_size = 256;
    if (out_size > CUPOLAS_SANITIZE_MAX_OUTPUT)
        out_size = CUPOLAS_SANITIZE_MAX_OUTPUT;

    char *buf = AIRY_MALLOC(out_size);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;

    int ret = cupolas_sanitize_input(params->input, buf, out_size);

    /* 严格模式下危险输入被拒绝（SANITIZE_REJECTED/ERROR 或 guard 拦截），
     * 输出缓冲区为空且返回非 0 —— 无净化产物，按拒绝处理（fail-closed）。 */
    if (ret != CUPOLAS_OK && buf[0] == '\0') {
        SVC_LOG_WARN("cupolas.sanitize: input rejected (rc=%d)", ret);
        AIRY_FREE(buf);
        return AIRY_ERR_PERMISSION_DENIED;
    }

    atomic_fetch_add_explicit(&svc->sanitize_count, 1, memory_order_relaxed);
    out->sanitized = buf; /* 调用方负责 cupolas_sanitize_result_free */
    out->err = ret;
    SVC_LOG_INFO("cupolas.sanitize: input_len=%zu rc=%d", in_len, ret);
    return AIRY_SUCCESS;
}

void cupolas_sanitize_result_free(cupolas_sanitize_result_t *out)
{
    if (!out)
        return;
    AIRY_FREE(out->sanitized);
    out->sanitized = NULL;
    out->err = 0;
}

/* ==================== 隔离工位命令执行 ==================== */

int cupolas_service_execute_command(cupolas_service_t *svc,
                                    const cupolas_execute_command_params_t *params,
                                    cupolas_execute_command_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->command || !params->argv)
        return AIRY_ERR_INVALID_PARAM;

    char *stdout_buf = AIRY_CALLOC(1, CUPOLAS_EXEC_OUTPUT_SIZE);
    char *stderr_buf = AIRY_CALLOC(1, CUPOLAS_EXEC_OUTPUT_SIZE);
    if (!stdout_buf || !stderr_buf) {
        AIRY_FREE(stdout_buf);
        AIRY_FREE(stderr_buf);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int exit_code = -1;
    int ret = cupolas_execute_command(params->command, params->argv, &exit_code,
                                      stdout_buf, CUPOLAS_EXEC_OUTPUT_SIZE,
                                      stderr_buf, CUPOLAS_EXEC_OUTPUT_SIZE);
    if (ret != CUPOLAS_OK) {
        AIRY_FREE(stdout_buf);
        AIRY_FREE(stderr_buf);
        SVC_LOG_ERROR("cupolas.execute_command failed: cmd=%s rc=%d", params->command, ret);
        return ret;
    }

    out->exit_code = exit_code;
    out->stdout_buf = stdout_buf; /* 调用方负责 cupolas_execute_command_result_free */
    out->stderr_buf = stderr_buf;
    out->err = 0;
    SVC_LOG_INFO("cupolas.execute_command: cmd=%s exit_code=%d", params->command, exit_code);
    return AIRY_SUCCESS;
}

void cupolas_execute_command_result_free(cupolas_execute_command_result_t *out)
{
    if (!out)
        return;
    AIRY_FREE(out->stdout_buf);
    AIRY_FREE(out->stderr_buf);
    out->stdout_buf = NULL;
    out->stderr_buf = NULL;
    out->exit_code = -1;
    out->err = 0;
}

/* ==================== 规则管理 ==================== */

int cupolas_service_add_rule(cupolas_service_t *svc,
                             const cupolas_add_rule_params_t *params,
                             cupolas_add_rule_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->resource)
        return AIRY_ERR_INVALID_PARAM;

    int ret = cupolas_add_permission_rule(params->agent_id, params->action, params->resource,
                                          params->allow, params->priority);
    if (ret != CUPOLAS_OK) {
        SVC_LOG_ERROR("cupolas.add_rule failed: resource=%s rc=%d", params->resource, ret);
        return ret;
    }

    out->added = 1;
    out->err = 0;
    SVC_LOG_INFO("cupolas.add_rule: agent=%s action=%s resource=%s allow=%d priority=%d",
                 params->agent_id ? params->agent_id : "*",
                 params->action ? params->action : "*",
                 params->resource, params->allow, params->priority);
    return AIRY_SUCCESS;
}

/* ==================== 审计 ==================== */

int cupolas_service_audit_flush(cupolas_service_t *svc)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;
    cupolas_flush_audit_log();
    SVC_LOG_INFO("cupolas.audit_flush: audit log flushed");
    return AIRY_SUCCESS;
}

/* ==================== 统计 ==================== */

char *cupolas_service_get_stats_json(cupolas_service_t *svc)
{
    if (!svc)
        return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    cJSON_AddStringToObject(obj, "daemon", "cupolas_d");
    cJSON_AddStringToObject(obj, "version", cupolas_version());
    cJSON_AddNumberToObject(obj, "uptime_s",
                            (double)((int64_t)time(NULL) - svc->start_time));
    cJSON_AddNumberToObject(obj, "permission_checks",
                            (double)atomic_load_explicit(&svc->permission_checks,
                                                         memory_order_relaxed));
    cJSON_AddNumberToObject(obj, "sanitize_count",
                            (double)atomic_load_explicit(&svc->sanitize_count,
                                                         memory_order_relaxed));

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}
