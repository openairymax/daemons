/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_alt.h
 * @brief C-25 等价工具表：主工具失败后「换哪条路」的策略判定。
 *
 *        本文件只定义契约与纯判定入口，不做任何执行：换路是同一预算内的
 *        一次替代调用，由 run loop 负责派发、事件与连败计数。表内只收录
 *        语义可证明等价的对，宁缺毋滥——把「跑不通就换个像的命令」做成
 *        默认行为，等于把不确定性伪装成韧性。
 *
 *        收录（当前三条，均可逐条复算）：
 *        1) shell_run(<特权程序> …) → priv_exec(program, argv)：同一程序、
 *           同一 argv，只换通道（通用 shell → 受审计特权通道）。即 dnf /
 *           rpm / systemctl 在 no_new_privs 或 sudo 不可用时的真实替代。
 *        2) shell_run(curl -s URL) → shell_run(wget -qO- URL)：同为「静默
 *           取回正文到标准输出」；仅接受 s/S/L 组合短旗标与单个 URL。
 *        3) 反向：shell_run(wget -qO- URL) → shell_run(curl -sSL URL)。
 *
 *        不收录：dnf ↔ rpm 的命令级对调。dnf 走仓库解析依赖、rpm 安装本地
 *        文件，二者替换会静默改变语义，属演示性功能，明确拒绝。
 */

#ifndef AIRY_RT_AGENT_RUN_ALT_H
#define AIRY_RT_AGENT_RUN_ALT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *tool; /* 替代工具名（拥有） */
    char *args; /* 替代参数 JSON（拥有） */
    char *note; /* 换路理由，写入 trace 供复盘（拥有） */
} run_alt_t;

/* 释放三个字段并把结构清零；对已清空的入参幂等。 */
void run_alt_free(run_alt_t *alt);

/* 换路开关：AIRY_TOOL_FALLBACK 取 off/0/false/no（不区分大小写）时关闭，
 * 其余（含未设置）开启。关闭即回到「失败只计数」的旧语义。 */
int run_alt_enabled(void);

/* 策略拒绝类错误码不得换路：换工具会绕过工具自报的拒绝面（例如沙箱拒绝
 * 被改道到特权通道），这是安全边界而不是重试策略。1 = 禁止换路。 */
int run_alt_deny(int code);

/* 按等价表为失败的调用求替代路：命中返回 1 并填满 out（三字段均拥有），
 * 未命中返回 0 且 out 保持清零。开关关闭、参数不可解析或不在表内均为 0。 */
int run_alt_plan(const char *tool, const char *args, run_alt_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_AGENT_RUN_ALT_H */
