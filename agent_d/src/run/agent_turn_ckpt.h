/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_turn_ckpt.h
 * @brief 回合检查点（D-12）：守护重启的可续跑状态契约。
 *
 * 工作大厅级持久化（coreloopthree work_hall_persist.c）只覆盖大厅条目；
 * agent 回合的推进状态驻留进程内存，守护重启即丢失。本模块在工具边界把
 * 「做到哪一步 + 续跑令牌」落到
 * `$AIRY_HOME/data/agentrt/state/agent_turn_ckpt.json`（tmp+fsync+rename
 * 原子写，与工作大厅同一落盘约定），客户端经 agent.turns 取回令牌原地续跑。
 *
 * 状态只有两态：
 *   - running：本进程正在推进（崩溃残留也停在此态）；
 *   - paused ：可续跑（启动恢复、退出排水、预算耗尽都会迁入）。
 *
 * 终态（完成/取消/失败）直接删除条目：只有非终态回合才需要恢复入口。
 */

#ifndef AIRY_RT_DAEMON_AGENT_TURN_CKPT_H
#define AIRY_RT_DAEMON_AGENT_TURN_CKPT_H

#include <stddef.h>

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_CKPT_MAX 8         /* 文件内保留回合数上限（崩溃残留不会无限增长） */
#define AGENT_CKPT_TOKEN_MAX 8192 /* 令牌长度上限（与引擎 TOKEN_MAX 同量级） */

/**
 * @brief 工具边界落盘：写入/更新该回合的可续跑状态。
 *
 * @param turn      回合标识（run_id；无 run_id 时用会话 ID）
 * @param sess      会话 ID（客户端续跑时按此匹配）
 * @param used      已用工具轮次
 * @param limit     本轮预算上限
 * @param tools_done 已完成工具调用数（trace 长度）
 * @param last_tool 最近一次工具名（可 NULL）
 * @param token     续跑令牌（非空；超长或空令牌视为不可续跑，不落盘）
 * @return 0 成功；负值路径/序列化失败
 */
int agent_ckpt_save(const char *turn, const char *sess, int used, int limit, int tools_done,
                    const char *last_tool, const char *token);

/** @brief 回合终态清理；排水期内保留为 paused（重启后可续）。 */
int agent_ckpt_done(const char *turn);

/** @brief 单回合置 paused（保留令牌）；条目不存在时返回 0。 */
int agent_ckpt_pend(const char *turn);

/**
 * @brief 守护启动恢复：上次进程遗留的 running 一律迁为 paused。
 * @return 迁入 paused 的条目数（无文件或无条目为 0）
 */
int agent_ckpt_boot(void);

/**
 * @brief 守护退出排水：置排水标记并把在途回合全部迁为 paused。
 * 排水标记生效后 agent_ckpt_save 不再写入、agent_ckpt_done 不再删除，
 * 保证最后一个工具边界的令牌可被重启后的客户端取回。
 * @return 置为 paused 的条目数
 */
int agent_ckpt_drain(void);

/** @brief 可续跑（paused）回合数。 */
int agent_ckpt_count(void);

/** @brief 渲染 agent.turns 的 result 对象（调用方 cJSON_Delete）；失败返回 NULL。 */
cJSON *agent_ckpt_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_TURN_CKPT_H */
