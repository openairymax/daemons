// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file think_service.h
 * @brief 双思考系统（Thinkdual）服务对外接口
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * think_d 承载 CoreLoopThree 认知引擎，将双思考系统接入 15 daemon 运行时：
 *   - t2（慢思考主模型）：TC3 批判循环主生成
 *   - t1-f（快思考-事实）：验证模型
 *   - t1-p（快思考-专业）：专家仲裁模型
 *   - dual_coordinate：双模型交叉验证（TC3 成功后激活，写入 working memory）
 *
 * LLM 调用经 llm_svc_adapter 直连 llm_d Unix socket（daemon_rpc_call），
 * 与 15 daemon 架构原生互通。
 */

#ifndef AIRY_RT_THINK_SERVICE_H
#define AIRY_RT_THINK_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 公共类型定义 ---------- */

typedef struct think_service think_service_t;

/**
 * @brief 双思考服务配置
 */
typedef struct {
    const char *s2_model;       /* t2 主思考模型名（NULL=provider 默认） */
    const char *verify_model;   /* t1-f 验证模型名（NULL=provider 默认） */
    const char *expert_model;   /* t1-p 仲裁模型名（NULL=provider 默认） */
    uint32_t process_timeout_ms; /* 单次认知处理超时（毫秒），0 用默认 120000 */
    uint32_t max_feedback_events; /* 思考事件环形缓冲容量，0 用默认 64 */
} think_service_config_t;

/**
 * @brief 思考处理结果（JSON 字符串，调用者 AIRY_FREE）
 *
 * 形如：
 * {
 *   "plan": {"task_plan_id","node_count","nodes":[{id,goal,handler,role,depends}]},
 *   "feedback": [{level,module,event,data}...],
 *   "stats": {"dual_thinking_enabled":1,"dual_invocations":N,"corrections":N}
 * }
 */
typedef struct {
    char *json;
    size_t json_len;
} think_process_result_t;

/* ---------- 生命周期 ---------- */

think_service_t *think_service_create(const think_service_config_t *config);
void think_service_destroy(think_service_t *svc);

/* ---------- 核心处理 ---------- */

/**
 * @brief 双思考处理：输入 prompt → 认知引擎（TC3 + dual_coordinate）→ JSON 结果
 * @param svc 服务句柄
 * @param prompt 用户输入（UTF-8）
 * @param out_result 输出结果（OWNER，调用者 think_result_free）
 * @return 0 成功，非 0 失败
 */
int think_service_process(think_service_t *svc, const char *prompt,
                          think_process_result_t *out_result);

void think_result_free(think_process_result_t *res);

/* ---------- 查询 ---------- */

/**
 * @brief 获取双思考统计（JSON 字符串，调用者 AIRY_FREE）
 * @param svc 服务句柄
 * @return JSON 字符串（含 dual_invocations/corrections/llm_backed 等），失败返回 NULL
 */
char *think_service_stats_json(think_service_t *svc);

/**
 * @brief 检查服务是否就绪（engine + adapter 均已创建）
 */
int think_service_ready(const think_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_THINK_SERVICE_H */
