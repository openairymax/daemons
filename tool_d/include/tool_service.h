/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_service.h
 * @brief 工具服务对外接口
 */

#ifndef AIRY_RT_TOOL_SERVICE_H
#define AIRY_RT_TOOL_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct tool_service tool_service_t;

/**
 * @brief 工具访问类型（改进1 P1d：并行工具并发门控）
 *
 * READ 工具只读无副作用 → 并发门控 read 锁（多工具并行执行）；
 * WRITE 工具有副作用 → 并发门控 write 锁（互斥串行执行）。
 * 枚举首项为 WRITE（=0），零初始化默认互斥串行（安全默认）。
 */
typedef enum {
    TOOL_ACCESS_WRITE = 0,
    TOOL_ACCESS_READ = 1,
} tool_access_t;

/**
 * @brief 工具参数定义（JSON Schema 格式字符串）
 */
typedef struct {
    const char *name;
    const char *schema;
    int required; /* 是否必需（0=可选，1=必需）。与 gateway 工具 schema 的
                         * required 数组一致（SSoT）：fs_list.path 可选（省略时
                         * 默认列出当前目录），fs_read/fs_write/shell_run 必填。 */
} tool_param_t;

/**
 * @brief 工具元数据
 */
typedef struct {
    char *id;
    char *name;
    char *description;
    char *executable;
    tool_param_t *params;
    size_t param_count;
    int timeout_sec;
    int cacheable;
    tool_access_t access;
    char *permission_rule;
} tool_metadata_t;

/**
 * @brief 工具执行请求
 */
typedef struct {
    const char *tool_id;
    const char *params_json;
    int stream;
    const char *agent_id;
    void *user_data;
} tool_execute_request_t;

/**
 * @brief 工具执行失败分级（改进3：Codex Fatal/RespondToModel/普通 三态）
 *
 * 上层（taskflow/工作大厅/蓝图调度）依据分级决定任务语义：
 *   - FATAL             → 终止任务（fail-closed），级联取消相关执行
 *   - RESPOND_TO_MODEL  → 结果回传上层，任务不终止（启动失败/审批拒绝等）
 *   - NORMAL_FAIL       → 封装 success:false 回传，任务继续（可配重试）
 */
typedef enum {
    TOOL_RESULT_CLASS_SUCCESS = 0,
    TOOL_RESULT_CLASS_FATAL,
    TOOL_RESULT_CLASS_RESPOND_TO_MODEL,
    TOOL_RESULT_CLASS_NORMAL_FAIL,
} tool_result_class_t;

/**
 * @brief 工具执行结果（非流式）
 */
typedef struct {
    int success;
    char *output;
    char *error;
    int exit_code;
    uint64_t duration_ms;
    tool_result_class_t failure_class;
} tool_result_t;

/**
 * @brief 流式输出回调
 * @param chunk  输出数据块
 * @param is_stderr 是否为错误输出
 * @param user_data 用户数据
 */
typedef void (*tool_stream_callback_t)(const char *chunk, int is_stderr, void *user_data);


tool_service_t *tool_service_create(const char *config_path);
void tool_service_destroy(tool_service_t *svc);


int tool_service_register(tool_service_t *svc, const tool_metadata_t *meta);
int tool_service_unregister(tool_service_t *svc, const char *tool_id);
tool_metadata_t *tool_service_get(tool_service_t *svc, const char *tool_id);
void tool_metadata_free(tool_metadata_t *meta);
char *tool_service_list(tool_service_t *svc);


int tool_service_execute(tool_service_t *svc, const tool_execute_request_t *req,
                         tool_result_t **out_result);

int tool_service_execute_stream(tool_service_t *svc, const tool_execute_request_t *req,
                                tool_stream_callback_t callback, void *callback_data,
                                tool_result_t **out_result);

void tool_result_free(tool_result_t *res);


/**
 * @brief 获取工具服务运行统计（L2 标准方法 tool.get_stats）
 * @param svc 工具服务实例
 * @return JSON 字符串（AIRY_MALLOC，调用者 AIRY_FREE），失败返回 NULL
 *
 * 返回字段：daemon、tools（注册工具数）、exec_total（执行总次数）、
 * exec_fail（失败次数）、exec_ms_total（累计耗时 ms）、avg_exec_ms。
 */
char *tool_service_get_stats(tool_service_t *svc);


/**
 * @brief 列出所有 pending 审批请求（JSON 数组字符串）
 * @param svc 工具服务实例
 * @return JSON 数组字符串（AIRY_MALLOC，调用者 AIRY_FREE），失败返回 NULL
 *
 * 每个元素: {request_id, tool, agent_id, params, created_at}
 */
char *tool_service_interactive_pending_list(tool_service_t *svc);

/**
 * @brief 按 request_id 决议一个 pending 审批请求
 * @param svc 工具服务实例
 * @param request_id 请求 ID
 * @param decision 决议："allow" / "always" / "deny"
 * @return 0 成功；未找到 AIRY_ERR_NOT_FOUND；参数非法 AIRY_ERR_INVALID_PARAM
 */
int tool_service_interactive_resolve(tool_service_t *svc, const char *request_id,
                                     const char *decision);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_SERVICE_H */