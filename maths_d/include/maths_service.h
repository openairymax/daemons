/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file maths_service.h
 * @brief 数学外挂计算服务核心（maths.* 命名空间）。
 *
 * 定位：Agent 运行时数学计算外挂的用户态服务（maths-toolkit market 包
 * 的调用方）。把数学计算从 LLM 推理中剥离（约 5 token 取代 150~300
 * token）。双引擎：
 *   - 纯 C 快速路径：基础算术/初等函数/统计（零外部依赖，毫秒级）；
 *   - Python 符号后端：solve/differentiate/integrate/limit/矩阵/单位
 *     换算（经 stdio JSON-RPC 调用 maths-toolkit 部署的 worker）。
 * Python 后端不可用时自动降级到纯 C 路径，不阻塞核心。
 *
 * 安全边界：数值求值不执行代码、不访问文件系统、不产生网络请求；
 * 表达式长度与解析深度硬限制，NaN/Inf 显式上报。
 */

#ifndef AIRY_RT_MATHS_SERVICE_H
#define AIRY_RT_MATHS_SERVICE_H

#include "platform.h"
#include "python_backend.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MATHS_MAX_EXPR_LEN 4096
#define MATHS_MAX_DEPTH 64
#define MATHS_MAX_VALUES 65536
#define MATHS_MAX_FUNC_NAME 32
#define MATHS_PLOT_MAX_SAMPLES 256 /* plot 采样上限（响应规模硬约束） */

/* RPC 域状态（协议无关）：OK 时 out_result 出 cJSON result；
 * ERR_METHOD 由 svc 层映射 -32601；ERR_DOMAIN 映射 -32000 基码 + err。 */
typedef enum {
    MATHS_RPC_OK = 0,
    MATHS_RPC_ERR_METHOD,
    MATHS_RPC_ERR_DOMAIN
} maths_rpc_status_t;

typedef struct {
    airy_mtx_t lock;
    atomic_int running;
    uint64_t start_time;
    uint64_t eval_count;      /* 成功求值次数 */
    uint64_t stats_count;     /* 统计调用次数 */
    uint64_t symbolic_count;  /* 符号计算调用次数 */
    uint64_t error_count;     /* 求值失败次数 */
    uint64_t last_eval_ms;    /* 最近一次求值耗时（毫秒） */
    maths_py_backend_t py_backend; /* Python 符号后端（maths-toolkit） */
} maths_d_service_t;

/**
 * @brief 初始化服务核心（计数器、锁、默认路径）。
 */
int maths_d_service_init(maths_d_service_t *svc);

/**
 * @brief 释放服务核心资源。
 */
void maths_d_service_destroy(maths_d_service_t *svc);

/**
 * @brief 表达式求值。expr 为数学表达式，out_result 指向结果缓冲。
 * @return 0 成功；非 0 失败（err_msg 填充可读错误）。
 */
int maths_d_eval(const char *expr, double *out_result, char *err_msg,
                 size_t err_msg_size);

/**
 * @brief 统计函数。op 支持 mean/median/variance/stddev/sum/max/min。
 * @return 0 成功；非 0 失败。
 */
int maths_d_stats(const char *op, const double *values, size_t count,
                  double *out_result, char *err_msg, size_t err_msg_size);

/**
 * @brief 表达式模式识别：判断文本是否应路由到数学外挂。
 * @return 1 是数学表达式；0 否。
 */
int maths_d_recognize(const char *text);

/**
 * @brief RPC 域调用（协议无关）：分发 method 并产出 result。
 * @param out_result 成功时填充 cJSON result（所有权移交调用方释放；
 *                   失败时置 NULL）。
 * @param err 域错误可读描述（ERR_METHOD 时不填充）。
 * @return MATHS_RPC_OK / MATHS_RPC_ERR_METHOD / MATHS_RPC_ERR_DOMAIN
 */
maths_rpc_status_t maths_d_rpc_call(maths_d_service_t *svc,
                                    const char *method, const cJSON *params,
                                    cJSON **out_result, char *err,
                                    size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MATHS_SERVICE_H */
