// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file svc_model_defaults.h
 * @brief model.yaml global / think 段默认模型提取（llm_d / gateway_d / think_d 共用）
 *
 * 用户模型配置 SSoT（ecosystem/manager/model/model.yaml）的 global 段包含
 * default_provider / default_model；think 段包含双思考（Thinkdual）三角色
 * 模型映射（s2 / verify / expert）。llm_d 消费该文件构建 provider registry；
 * gateway_d 需要同样的默认模型语义（AI Agent 编排缺省模型）；think_d 需要
 * think 段完成 t2/t1-f/t1-p 三模型注入。本 API 将 "从 YAML 提取配置段"
 * 统一收敛在 daemon 公共层，避免各 daemon 各自实现 libyaml 扫描逻辑
 * （单一来源）。
 *
 * 同时支持用户覆盖文件 $AIRY_CONFIG_DIR/model.yaml（与仓库 SSoT 合并时
 * 同名 global 字段用户优先），二者都通过本 API 读取后由调用方决定优先级。
 */

#ifndef SVC_MODEL_DEFAULTS_H
#define SVC_MODEL_DEFAULTS_H

#include <stddef.h>
#include <stdint.h>

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

/**
 * @brief 双思考（Thinkdual）三模型配置（model.yaml 的 think 段）
 *
 * enabled=1 时 think_d 启用 GRAD 计划级批判循环；三模型名空串表示
 * 使用 provider 默认（global.default_model）。timeout_ms=0 表示使用
 * think_d 内置默认（120000）。
 */
typedef struct {
    int enabled;                /* think.enabled（默认 1） */
    char think2_slow_model[128]; /* think.think2_slow_model（t2 慢思考，生成计划） */
    char think1_fast_model[128]; /* think.think1_fast_model（t1-f 快思考，语境终裁） */
    char think1_prof_model[128]; /* think.think1_prof_model（t1-p 专业思考，四验专家） */
    uint32_t timeout_ms;        /* think.timeout_ms（单次认知处理超时） */
} svc_model_think_config_t;

/**
 * @brief 从 model.yaml 的 think 段提取双思考三模型配置
 *
 * 与 svc_model_defaults_from_yaml 同源状态机：仅关注顶级 think: mapping
 * 内的 enabled / think2_slow_model / think1_fast_model / think1_prof_model /
 * timeout_ms 键。
 * 未找到 think 段时 out 保持初始值（enabled=1，模型名空，timeout=0），
 * 不视为错误（旧配置兼容）。
 *
 * @param path YAML 文件路径
 * @param out  输出结构（非 NULL；调用方应先行清零）
 * @return 0 成功；AIRY_ERR_INVALID_PARAM / AIRY_ERR_IO / AIRY_ERR_NOT_SUPPORTED
 */
int svc_model_defaults_think_from_yaml(const char *path,
                                       svc_model_think_config_t *out);

/**
 * @brief 简化 LLM 配置（model.yaml 的 llm 段）
 *
 * 面向普通用户的最小配置入口：一个 provider + 一个默认模型。
 * api_format 仅两种：openai（OpenAI Chat Completions）/ anthropic
 * （Anthropic Messages）。base_url 为适配器根地址（DeepSeek 官方同源
 * 双协议：openai 形如 https://api.deepseek.com；anthropic 形如
 * https://api.deepseek.com/anthropic，见 api-docs.deepseek.com）。
 * 存在 llm 段且 model 非空时，llm_d 运行时展开为单 provider（优先于完整
 * providers/models 列表）。
 */
typedef struct {
    char api_format[16];    /* llm.api_format: openai | anthropic */
    char base_url[512];     /* llm.base_url（适配器根地址） */
    char api_key_env[128];  /* llm.api_key_env（对应 secrets.env 变量名） */
    char model[128];        /* llm.model（默认模型名） */
} svc_model_llm_config_t;

/**
 * @brief 从 model.yaml 的 llm 段提取简化 LLM 配置
 *
 * 同源 libyaml 状态机：仅关注顶级 llm: mapping 内的 api_format /
 * base_url / api_key_env / model 键。未找到 llm 段时 out 保持调用方
 * 初始值，不视为错误。
 *
 * @param path YAML 文件路径
 * @param out  输出结构（非 NULL；调用方应先行清零）
 * @return 0 成功；AIRY_ERR_INVALID_PARAM / AIRY_ERR_IO / AIRY_ERR_NOT_SUPPORTED
 */
int svc_model_defaults_llm_from_yaml(const char *path,
                                     svc_model_llm_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SVC_MODEL_DEFAULTS_H */
