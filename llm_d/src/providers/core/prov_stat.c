// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file prov_stat.c
 * @brief Provider 传输侧观测机制件（llm_d providers/core，0.1.20 D2-9）。
 *
 * 传输层有两类事实必须被观测到才能被治理：一是超时成因，二是正常请求的
 * 时延分布。前者决定重试与告警策略（连接未发出可安全重试，中途停摆只在
 * 幂等时重试，总预算耗尽永不重试），后者决定整轮预算该给多大——常量
 * 120s 在慢端点上必然误杀，在快端点上又白等。
 *
 * 本件只提供"记一笔、读一笔"的机制：三类超时计数以原子量累计，时延以
 * 每 provider 的定长环形缓冲采样，p99 由调用侧现算。预算如何由 p99 推导
 * （倍数、上下限、无样本缺省）作为策略常量集中在本文件，便于单点调整。
 *
 * 并发：计数路径无锁（原子量）；采样路径以懒初始化的 mutex 串行化，
 * 临界区内只做拷贝，排序在锁外完成，绝不持锁做重活。
 */

#include "platform_sync.h"
#include "prov_stat.h"
#include "sync.h"

#include <string.h>

#define PROV_TMO_FLOOR_SEC 120.0  /* 预算下限：低于此值对生成类请求无意义 */
#define PROV_TMO_CAP_SEC 180.0    /* 无样本缺省：方案 §5.2b.10 的保守值 */
#define PROV_TMO_SCALE 1.5        /* p99 之上的安全裕度 */
#define PROV_NAME_MAX 64

typedef struct {
    char name[PROV_NAME_MAX];
    uint32_t samples[PROV_STAT_SLOTS];
    uint32_t head;
    uint32_t count;
} prov_lat_t;

static volatile uintptr_t g_tmo[PROV_TMO_KIND_N];
static prov_lat_t g_lat[PROV_STAT_GROUPS];
static airy_mtx_t g_lat_lock;
static airy_once_t g_lat_once = AIRY_ONCE_INIT;

static void lat_lock_init(void)
{
    airy_mtx_init(&g_lat_lock);
}

static void lat_guard(void)
{
    airy_call_once(&g_lat_once, lat_lock_init);
}

/* 调用方须持 g_lat_lock。create=1 时按需占用空槽。 */
static prov_lat_t *lat_slot(const char *provider, int create)
{
    int free_slot = -1;

    for (int i = 0; i < PROV_STAT_GROUPS; i++) {
        if (g_lat[i].name[0] == '\0') {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (strncmp(g_lat[i].name, provider, PROV_NAME_MAX - 1) == 0) {
            return &g_lat[i];
        }
    }
    if (!create || free_slot < 0) {
        return NULL;
    }
    prov_lat_t *slot = &g_lat[free_slot];
    size_t n = 0U;
    while (n + 1U < (size_t)PROV_NAME_MAX && provider[n] != '\0') {
        slot->name[n] = provider[n];
        n++;
    }
    slot->name[n] = '\0';
    slot->head = 0U;
    slot->count = 0U;
    return slot;
}

void prov_stat_hit(prov_tmo_t kind)
{
    if ((int)kind < 0 || (int)kind >= (int)PROV_TMO_KIND_N) {
        return;
    }
    sync_atomic_add(&g_tmo[kind], 1U);
}

uint64_t prov_stat_get(prov_tmo_t kind)
{
    if ((int)kind < 0 || (int)kind >= (int)PROV_TMO_KIND_N) {
        return 0U;
    }
    return (uint64_t)sync_atomic_load(&g_tmo[kind]);
}

void prov_stat_lat(const char *provider, uint32_t ms)
{
    if (provider == NULL || provider[0] == '\0') {
        return;
    }
    lat_guard();
    airy_mtx_lock(&g_lat_lock);
    prov_lat_t *slot = lat_slot(provider, 1);
    if (slot != NULL) {
        slot->samples[slot->head] = ms;
        slot->head = (slot->head + 1U) % (uint32_t)PROV_STAT_SLOTS;
        if (slot->count < (uint32_t)PROV_STAT_SLOTS) {
            slot->count++;
        }
    }
    airy_mtx_unlock(&g_lat_lock);
}

double prov_stat_p99(const char *provider)
{
    uint32_t buf[PROV_STAT_SLOTS];
    uint32_t n = 0U;

    if (provider == NULL || provider[0] == '\0') {
        return 0.0;
    }
    lat_guard();
    airy_mtx_lock(&g_lat_lock);
    prov_lat_t *slot = lat_slot(provider, 0);
    if (slot != NULL) {
        n = slot->count;
        for (uint32_t i = 0; i < n; i++) {
            buf[i] = slot->samples[i];
        }
    }
    airy_mtx_unlock(&g_lat_lock);

    if (n == 0U) {
        return 0.0;
    }
    for (uint32_t i = 1; i < n; i++) { /* 插入排序，n <= PROV_STAT_SLOTS */
        uint32_t key = buf[i];
        uint32_t j = i;
        while (j > 0U && buf[j - 1U] > key) {
            buf[j] = buf[j - 1U];
            j--;
        }
        buf[j] = key;
    }
    uint32_t idx = (uint32_t)(((uint64_t)n * 99U + 99U) / 100U) - 1U;
    return (double)buf[idx];
}

double prov_tmo_auto(const char *provider)
{
    double p99 = prov_stat_p99(provider);
    if (p99 <= 0.0) {
        return PROV_TMO_CAP_SEC;
    }
    double scaled = p99 * PROV_TMO_SCALE;
    return scaled > PROV_TMO_FLOOR_SEC ? scaled : PROV_TMO_FLOOR_SEC;
}

void prov_stat_reset(void)
{
    for (int i = 0; i < (int)PROV_TMO_KIND_N; i++) {
        sync_atomic_store(&g_tmo[i], 0U);
    }
    lat_guard();
    airy_mtx_lock(&g_lat_lock);
    /* 只复位身份与游标：样本区由 count 门控，无需清零。 */
    for (int i = 0; i < PROV_STAT_GROUPS; i++) {
        g_lat[i].name[0] = '\0';
        g_lat[i].head = 0U;
        g_lat[i].count = 0U;
    }
    airy_mtx_unlock(&g_lat_lock);
}
