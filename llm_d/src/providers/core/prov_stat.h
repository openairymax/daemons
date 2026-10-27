/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* prov_stat.h: Provider 传输侧观测计数与超时自适应（SSoT，D2-9） */

#ifndef LLM_D_PROVIDERS_CORE_PROV_STAT_H
#define LLM_D_PROVIDERS_CORE_PROV_STAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROV_STAT_SLOTS 64 /* 每 provider 保留的最近时延样本数 */
#define PROV_STAT_GROUPS 8 /* 时延采样覆盖的 provider 数上限 */

/* 超时三拆：三类互斥的超时成因，分别计数、分别处置。
 * CONNECT 请求未送达（可安全重试）；STALL 上游中途停摆（幂等时可重试）；
 * TOTAL 整轮预算耗尽（永不重试）。 */
typedef enum {
    PROV_TMO_CONNECT = 0,
    PROV_TMO_STALL = 1,
    PROV_TMO_TOTAL = 2,
    PROV_TMO_KIND_N = 3
} prov_tmo_t;

void prov_stat_hit(prov_tmo_t kind);
uint64_t prov_stat_get(prov_tmo_t kind);
void prov_stat_lat(const char *provider, uint32_t ms);
double prov_stat_p99(const char *provider);
/* 无样本时给 180s（方案 §5.2b.10 的保守缺省），有样本时 max(120, p99*1.5)。 */
double prov_tmo_auto(const char *provider);
void prov_stat_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROV_STAT_H */
