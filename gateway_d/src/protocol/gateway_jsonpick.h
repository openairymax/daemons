// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gateway_jsonpick.h
 * @brief 网关协议请求体 JSON 字段提取机制（唯一实现）。
 *
 * 0.1.19 t45：protocol 层手写 strstr 字段提取器十五副本消解的机制载体。
 * 解析与取值统一走 cJSON，消除手写扫描对字符串值内嵌键名、嵌套同名
 * 字段与转义序列的误匹配；缺省回退（"{}" 兜底、备用键、数值默认）等
 * 策略由各协议 handler 自行决定（机制与策略分离）。
 *
 * 约定：root 为 NULL 等价于"无此字段"；所有返回字符串为 AIRY_MALLOC
 * 副本，调用方以 AIRY_FREE 释放。
 */

#ifndef GATEWAY_JSONPICK_H
#define GATEWAY_JSONPICK_H

#include <cjson/cJSON.h>

/* 解析请求体；json 为空或非法 JSON 返回 NULL */
cJSON *gw_json_load(const char *json);

/* string 字段值副本；缺失或非 string 返回 NULL */
char *gw_json_str(const cJSON *root, const char *field);

/* 字段值 JSON 原文：string 带引号、object/array 规范化文本、数字原样；
 * 缺失或值为 bool/null 返回 NULL */
char *gw_json_raw(const cJSON *root, const char *field);

/* 数值字段；缺失或非数值返回 def */
double gw_json_num(const cJSON *root, const char *field, double def);

/* 整数字段；缺失或非数值返回 def */
int gw_json_int(const cJSON *root, const char *field, int def);

#endif /* GATEWAY_JSONPICK_H */
