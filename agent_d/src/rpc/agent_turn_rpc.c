// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_turn_rpc.c
 * @brief agent.turns：回合检查点只读出口（D-12）。
 *
 * 守护重启（或被中断）后客户端凭此取回 paused 回合的续跑令牌，原地续跑；
 * 无 paused 条目时返回空表，客户端如实告知不可续跑。本方法只读，不触发
 * 任何状态迁移——恢复语义由 agent_ckpt_boot/agent_ckpt_drain 负责。
 */

#include "run/agent_turn_ckpt.h"
#include "daemon_main.h"

#include <cjson/cJSON.h>

static void handle_turns(int id, airy_sock_t client_fd)
{
    cJSON *result = agent_ckpt_dump();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "turn checkpoint unavailable", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

DAEMON_RPC_SHELL0(turns, handle_turns)
