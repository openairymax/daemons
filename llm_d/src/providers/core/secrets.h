/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file secrets.h
 * @brief Provider 域密钥机制层（B16-S3 c3 自 provider.c 收敛）。
 *
 * 全域唯一 explicit_bzero 实现与 api_key 生命周期 SSoT：解析（env: 展开）、
 * 热重载（secrets.env）、env 回落名推导。适配层禁止自造擦除副本或自行
 * 读 secrets.env；env 回落名按品牌无关统一约定（厂商名大写 + "_API_KEY"）
 * 推导，core/ 内不持有任何厂商硬表或厂商特判分支。
 */

#ifndef LLM_D_PROVIDERS_CORE_SECRETS_H
#define LLM_D_PROVIDERS_CORE_SECRETS_H

#include "transport.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* macOS 严格 feature 宏（-std=c99 等）下 <string.h> 不声明 explicit_bzero，
 * Windows UCRT 亦无。系统若有该函数声明（glibc），它不是宏，此处检测不
 * 冲突，#define 把后续调用统一改为 volatile 擦除，敏感内存清零不被优化器
 * 消去。 */
#ifndef explicit_bzero
static inline void airy_sec_bzero(void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (n-- > 0) {
        *p++ = 0;
    }
}
#define explicit_bzero(s, n) airy_sec_bzero((s), (n))
#endif

/* "env:NAME" 展开为 getenv(NAME)；明文原样返回；空/非法返回 NULL。 */
const char *sec_resolve_key(const char *api_key);

/* 厂商名 → 标准环境变量名（品牌无关统一约定：厂商名大写、非字母数字归一
 * 为 '_'，追加 "_API_KEY"），结果写入 out（含结尾 NUL）。out_size 不足或
 * 入参非法时返回 false 且不写 out。 */
bool sec_env_name_for(const char *provider_name, char *out, size_t out_size);

/* Hot reload: called before each request; if the current api_key is empty,
 * fills it from $AIRY_HOME/config/secrets.env using base_ctx->api_key_env
 * (keys filled after startup need no restart). */
void provider_refresh_api_key(provider_base_ctx_t *base_ctx);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_SECRETS_H */
