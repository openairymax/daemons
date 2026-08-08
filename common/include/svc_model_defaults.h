// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file svc_model_defaults.h
 * @brief model.yaml global 段默认模型提取（llm_d / gateway_d 共用）
 *
 * 用户模型配置 SSoT（ecosystem/manager/model/model.yaml）的 global 段包含
 * default_provider / default_model。llm_d 消费该文件构建 provider registry；
 * gateway_d 需要同样的默认模型语义（AI Agent 编排缺省模型）。本 API 将
 * "从 YAML 提取 global 段"统一收敛在 daemon 公共层，避免两个 daemon
 * 各自实现 libyaml 扫描逻辑（单一来源）。
 *
 * 同时支持用户覆盖文件 $AIRY_CONFIG_DIR/model.yaml（与仓库 SSoT 合并时
 * 同名 global 字段用户优先），二者都通过本 API 读取后由调用方决定优先级。
 */

#ifndef SVC_MODEL_DEFAULTS_H
#define SVC_MODEL_DEFAULTS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 model.yaml 的 global 段提取 default_model / default_provider
 *
 * libyaml 事件流扫描：仅关注顶级 global: mapping 内的 default_model 与
 * default_provider 键值。字段缺失时对应输出缓冲保持为空串（不视为错误）；
 * 文件不存在/不可读返回 AIRY_ERR_IO；未编译 libyaml 支持返回
 * AIRY_ERR_NOT_SUPPORTED。
 *
 * @param path          YAML 文件路径
 * @param out_model     输出 default_model 缓冲（可为 NULL，跳过该字段）
 * @param model_sz      out_model 缓冲大小
 * @param out_provider  输出 default_provider 缓冲（可为 NULL，跳过该字段）
 * @param prov_sz       out_provider 缓冲大小
 * @return 0 成功；AIRY_ERR_INVALID_PARAM / AIRY_ERR_IO / AIRY_ERR_NOT_SUPPORTED
 */
int svc_model_defaults_from_yaml(const char *path,
                                 char *out_model, size_t model_sz,
                                 char *out_provider, size_t prov_sz);

#ifdef __cplusplus
}
#endif

#endif /* SVC_MODEL_DEFAULTS_H */
