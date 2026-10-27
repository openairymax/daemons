/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_swap.h
 * @brief 配置热切换的排空闸门与代际（0.1.20 D2-10）。
 *
 * 配置一旦可热重载，旧配置上的在途请求与新配置就会并存：同一个请求可能
 * 在旧权重上开始、在新权重上收尾，产出的内容两段不相接。本机制件给出
 * 唯一裁决点，与具体配置无关：
 *
 *   取号   svc_swap_enter()  排空中即拒绝（AIRY_ERR_BUSY），否则 inflight++
 *   归还   svc_swap_leave()  世代一致保留结果；不一致即丢弃并记 gen_mismatch
 *   排空   svc_swap_begin()  置 draining，等在途清零或超 drain_ms（超时记
 *                            drain_timeout，返回强制切换）
 *   提交   svc_swap_commit() 世代 +1，撤销 draining，唤醒等待者
 *   放弃   svc_swap_abort()  撤销 draining，世代不变（旧配置继续服务）
 *
 * 语义裁定：
 *   - 排空窗口按「在途请求数」而非请求速率判定，与业务形态无关
 *   - drain 超时不是失败：旧配置必须让位，超时请求在归还时被世代否决，
 *     由调用方重试，绝不允许混用两代结果
 *   - 世代从 1 起，0 保留为「未取号」，避免全零结构被误认为合法世代
 *
 * 线程安全：除 stat 外的全部调用可并发；排空判定在内部锁内完成。
 */

#ifndef AIRY_RT_DAEMONS_COMMON_SVC_SWAP_H
#define AIRY_RT_DAEMONS_COMMON_SVC_SWAP_H

#include <stdint.h>

#include "platform_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 默认排空窗口：冷启动慢推理的一次请求通常在此期间结束，再长只会拖住
 * 配置生效。 */
#define SVC_SWAP_DRAIN_MS 30000U

typedef struct {
    airy_mtx_t lock;
    airy_cond_t cv;
    uint32_t gen;          /* 当前世代，从 1 起 */
    uint32_t inflight;     /* 已取号未归还的请求数 */
    uint32_t drain_ms;     /* 排空上限 */
    int draining;          /* 1 = 正在排空，拒新请求 */
    uint32_t swapped;      /* 成功切换次数 */
    uint64_t mismatch;     /* 世代不符被丢弃的结果数 */
    uint64_t drain_tmo;    /* 排空超时（强制切换）次数 */
} svc_swap_t;

/**
 * @brief 初始化闸门
 * @param drain_ms 排空上限，0 取 SVC_SWAP_DRAIN_MS
 * @return 0 成功；非 0 平台原语失败
 */
int svc_swap_init(svc_swap_t *s, uint32_t drain_ms);

/** @brief 释放闸门内部原语（不释放 s 本身） */
void svc_swap_done(svc_swap_t *s);

/**
 * @brief 请求取号
 * @param gen 出参：本次请求所属世代
 * @return 0 取号成功；AIRY_ERR_BUSY 排空中（调用方应稍后重试）
 */
int svc_swap_enter(svc_swap_t *s, uint32_t *gen);

/**
 * @brief 请求归还并裁决结果是否仍有效
 * @return 1 世代一致，结果可用；0 世代已被切换，结果必须丢弃
 */
int svc_swap_leave(svc_swap_t *s, uint32_t gen);

/**
 * @brief 声明切换意图并排空在途请求
 * @return 0 已排空；1 排空超时（调用方须强制切换）；AIRY_ERR_BUSY 他人正在切换
 */
int svc_swap_begin(svc_swap_t *s);

/** @brief 切换成功：世代 +1 并恢复接客 */
int svc_swap_commit(svc_swap_t *s);

/** @brief 放弃切换：撤销 draining，世代与旧配置不变 */
void svc_swap_abort(svc_swap_t *s);

/** @brief 读计数快照（任一出参可为 NULL） */
void svc_swap_stat(const svc_swap_t *s, uint32_t *gen, uint32_t *inflight, uint32_t *swapped,
                   uint64_t *mismatch, uint64_t *drain_tmo);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMONS_COMMON_SVC_SWAP_H */
