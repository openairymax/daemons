// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file mem_service.h
 * @brief Memory 服务对外接口（mem.* 命名空间）
 *
 * 承载原 syscall_router.c 中 airy_sys_memory_write/search/get/delete
 * 的运行时记忆管理逻辑，作为 mem_d 守护进程的服务核心对外暴露。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AIRY_RT_MEM_SERVICE_H
#define AIRY_RT_MEM_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mem_service mem_service_t;

/**
 * @brief 记忆写入参数
 */
typedef struct {
    const void *data;       /* 数据指针 */
    size_t len;             /* 数据长度（字节） */
    const char *metadata;   /* JSON 元数据字符串，可为 NULL */
} mem_write_request_t;

/**
 * @brief 记忆检索结果项
 */
typedef struct {
    char *record_id;        /* 记录 ID（调用方负责 AIRY_FREE） */
    float score;            /* 相关度分数 [0.0, 1.0] */
} mem_search_hit_t;

/**
 * @brief 记录读取结果
 */
typedef struct {
    void *data;             /* 数据指针（调用方负责 AIRY_FREE） */
    size_t len;             /* 数据长度 */
    char *metadata;         /* 元数据 JSON 字符串（调用方负责 AIRY_FREE） */
} mem_record_t;

/* ---------- 生命周期 ---------- */

mem_service_t *mem_service_create(size_t max_records);
void mem_service_destroy(mem_service_t *svc);

/* ---------- 记忆管理接口 ---------- */

/**
 * @brief 写入记忆记录
 * @return AIRY_SUCCESS 成功，*out_record_id 输出新记录 ID（调用方负责 AIRY_FREE）
 */
int mem_service_write(mem_service_t *svc, const mem_write_request_t *req,
                       char **out_record_id);

/**
 * @brief 检索记忆记录（按相关性倒序）
 * @return AIRY_SUCCESS 成功，*out_hits 输出命中数组，*out_count 输出命中数
 */
int mem_service_search(mem_service_t *svc, const char *query, uint32_t limit,
                        mem_search_hit_t **out_hits, size_t *out_count);

/**
 * @brief 读取记忆记录
 * @return AIRY_SUCCESS 成功，*out_record 输出记录内容（调用方负责 mem_record_free）
 */
int mem_service_get(mem_service_t *svc, const char *record_id,
                     mem_record_t *out_record);

/**
 * @brief 删除记忆记录
 */
int mem_service_delete(mem_service_t *svc, const char *record_id);

/* ---------- 辅助接口 ---------- */

size_t mem_service_count(mem_service_t *svc);
void mem_search_hits_free(mem_search_hit_t *hits, size_t count);
void mem_record_free(mem_record_t *rec);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MEM_SERVICE_H */
