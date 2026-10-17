/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file python_backend.h
 * @brief maths_d 的 Python 数学后端管理（maths-toolkit market 包）。
 *
 * maths_d 是调用数学工具包的用户态服务。本模块负责：
 *   - 检测共享虚拟环境（$AIRY_HOME/venv）与后端脚本
 *     （$AIRY_HOME/backend/maths_backend.py，由 maths-toolkit 安装器部署）；
 *   - 以 stdio JSON-RPC 方式与常驻 Python worker 通信；
 *   - worker 崩溃时自动重启（有限次数）。
 *
 * 仅 POSIX（Windows 上 maths_d 走 TCP 回退，不启用 Python 后端）。
 */

#ifndef AIRY_RT_PYTHON_BACKEND_H
#define AIRY_RT_PYTHON_BACKEND_H

#include "platform.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MATHS_BACKEND_TIMEOUT_MS 8000
#define MATHS_BACKEND_RESET_MAX 2

typedef struct {
    int in_fd;                  /* 写入 worker stdin */
    int out_fd;                 /* 读取 worker stdout */
    int available;              /* 后端可用（venv + 脚本 + worker 存活） */
    int respawn_count;          /* 连续重启次数（超限则降级） */
    char python_path[512];      /* 解释器路径（venv 优先） */
    char backend_path[512];     /* maths_backend.py 路径 */
} maths_py_backend_t;

/**
 * @brief 初始化并拉起 Python 数学后端。
 * @param airy_home AIRY_HOME 路径（定位 venv 与 backend 脚本）
 * @return 0 可用；非 0 不可用（不阻塞，调用方降级到纯 C 路径）
 */
int maths_backend_init(maths_py_backend_t *be, const char *airy_home);

/**
 * @brief 经 stdio JSON-RPC 调用后端方法。
 * @param method    方法名（solve/differentiate/integrate/...）
 * @param params_json 方法参数 JSON 对象字符串（无外层花括号也可）
 * @param resp      输出缓冲（完整 JSON 响应行）
 * @param resp_sz   缓冲大小
 * @return 0 成功；非 0 失败（worker 不可用/超时/协议错误）
 */
int maths_backend_call(maths_py_backend_t *be, const char *method,
                       const char *params_json, char *resp, size_t resp_sz);

/**
 * @brief 后端是否可用。
 */
int maths_backend_available(const maths_py_backend_t *be);

/**
 * @brief 释放后端（终止 worker 子进程）。
 */
void maths_backend_destroy(maths_py_backend_t *be);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_PYTHON_BACKEND_H */
