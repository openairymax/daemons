/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file contract.h
 * @brief llm_d 端点与服务端容量常量（进程装配域内唯一事实源）。
 *
 * 由 llm_service_internal.h（253 行枢纽头，B16-S1 拆片）迁入。
 * 仅 bootstrap 域消费；跨域禁止 include 本头。
 */

#ifndef AIRY_RT_LLM_BOOTSTRAP_CONTRACT_H
#define AIRY_RT_LLM_BOOTSTRAP_CONTRACT_H

#include "platform_paths.h"

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("llm.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_llm"
#define DEFAULT_TCP_PORT 8080
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64
#define MAX_THREADS 8
#define MAX_MESSAGES_PER_REQUEST 128

#endif /* AIRY_RT_LLM_BOOTSTRAP_CONTRACT_H */
