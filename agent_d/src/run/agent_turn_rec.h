/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_turn_rec.h
 * @brief 会话记忆记录构造：一轮问答两条记录，role 与 conv 均显式。
 *
 * 写读契约的唯一实现点。此前 agent_d 把一轮问答压成单条
 * "user: ...\nassistant: ..." 且 role 记为 agentrt、不带 conv，TUI 重启
 * 恢复时既无法按会话归组（缺 conv），也无法识别角色（role 不在枚举内），
 * 助手回复在会话布局里永久缺席。
 *
 * 现约定：用户输入与助手回复各为一条 mem.write 参数，data 为原文，
 * metadata 携带 role（user/assistant 枚举）与 conv（会话标识，与
 * session_id 同值）。该函数只做纯构造，不触达传输层。
 */

#ifndef AIRY_RT_DAEMON_AGENT_TURN_REC_H
#define AIRY_RT_DAEMON_AGENT_TURN_REC_H

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 构造本轮问答的 mem.write 参数数组。
 *
 * @param conv  会话标识（与 session_id 同值，可为 NULL）。
 * @param user  用户输入原文；为空时返回 NULL（无可持久化内容）。
 * @param reply 助手回复原文；为空时只产生用户一条。
 * @return cJSON 数组（1~2 个对象，调用方 cJSON_Delete 整树）；失败或
 *         无用户输入时返回 NULL。
 */
cJSON *agent_turn_rec(const char *conv, const char *user, const char *reply);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_TURN_REC_H */
