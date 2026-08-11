/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file plugin_service.h
 * @brief Plugin 守护进程服务接口
 *
 * Plugin daemon 管理动态插件的加载、卸载、生命周期和沙箱。
 * 支持的插件类型：
 *   - TOOL_PROVIDER:  工具提供者插件
 *   - PROTOCOL_ADAPTER: 协议适配器插件
 *   - MEMORY_PROVIDER:  记忆提供商插件
 *   - HOOK_EXTENSION:   Hook 扩展插件
 *
 * @owner team-A
 * @see contracts/contract_A_B.h 第3节（协议适配器 vtable）
 */

#ifndef AIRY_RT_DAEMON_PLUGIN_D_PLUGIN_SERVICE_H
#define AIRY_RT_DAEMON_PLUGIN_D_PLUGIN_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    PLUGIN_TYPE_TOOL_PROVIDER = 0,
    PLUGIN_TYPE_PROTOCOL_ADAPTER = 1,
    PLUGIN_TYPE_MEMORY_PROVIDER = 2,
    PLUGIN_TYPE_HOOK_EXTENSION = 3,
    PLUGIN_TYPE_COUNT = 4
} plugin_type_t;


typedef enum {
    PLUGIN_STATE_UNLOADED = 0,
    PLUGIN_STATE_LOADED = 1,
    PLUGIN_STATE_INITIALIZED = 2,
    PLUGIN_STATE_RUNNING = 3,
    PLUGIN_STATE_ERROR = 4,
    PLUGIN_STATE_DISABLED = 5,
    PLUGIN_STATE_STARTING = 6
} plugin_state_t;


typedef struct {
    char name[64];
    char version[32];
    char author[64];
    char description[256];
    plugin_type_t type;
    uint32_t api_version;
    uint32_t min_airy_version;
} plugin_metadata_t;


/**
 * @brief 插件初始化回调
 * @param config_path 配置文件路径
 * @param user_data   用户数据输出
 * @return 0 成功，非0失败
 */
typedef int (*plugin_init_fn)(const char *config_path, void **user_data);

/**
 * @brief 插件销毁回调
 * @param user_data 用户数据
 */
typedef void (*plugin_destroy_fn)(void *user_data);

/**
 * @brief 插件启动回调
 * @param user_data 用户数据
 * @return 0 成功，非0失败
 */
typedef int (*plugin_start_fn)(void *user_data);

/**
 * @brief 插件停止回调
 * @param user_data 用户数据
 * @return 0 成功，非0失败
 */
typedef int (*plugin_stop_fn)(void *user_data);


typedef struct {
    plugin_metadata_t metadata;
    plugin_init_fn init;
    plugin_destroy_fn destroy;
    plugin_start_fn start;
    plugin_stop_fn stop;
    void *handle;
    void *user_data;
    plugin_state_t state;
    char config_path[256];
    char library_path[256];
} plugin_descriptor_t;


typedef struct {
    uint64_t load_count;
    uint64_t error_count;
    uint64_t uptime_ns;
    uint64_t memory_bytes;
} plugin_stats_t;


/**
 * @brief 从动态库加载插件
 * @param library_path 动态库路径
 * @param config_path  配置文件路径
 * @param out_name     输出插件名称
 * @return 0 成功，非0失败
 */
int plugin_service_load(const char *library_path, const char *config_path, const char **out_name);

/**
 * @brief 卸载插件
 * @param name 插件名称
 * @return 0 成功，非0失败
 */
int plugin_service_unload(const char *name);

/**
 * @brief 启动插件
 * @param name 插件名称
 * @return 0 成功，非0失败
 */
int plugin_service_start(const char *name);

/**
 * @brief 停止插件
 * @param name 插件名称
 * @return 0 成功，非0失败
 */
int plugin_service_stop(const char *name);

/**
 * @brief 获取插件元数据
 * @param name     插件名称
 * @param metadata 输出元数据
 * @return 0 成功，非0失败
 */
int plugin_service_get_metadata(const char *name, plugin_metadata_t *metadata);

/**
 * @brief 获取插件状态
 * @param name  插件名称
 * @return 插件状态
 */
plugin_state_t plugin_service_get_state(const char *name);

/**
 * @brief 获取插件统计
 * @param name  插件名称
 * @param stats 输出统计
 * @return 0 成功，非0失败
 */
int plugin_service_get_stats(const char *name, plugin_stats_t *stats);

/**
 * @brief 列出所有已加载插件
 * @param names    输出名称数组（需调用者释放）
 * @param count    输出数量
 * @param type_filter 类型过滤（-1 表示所有类型）
 * @return 0 成功，非0失败
 */
int plugin_service_list(char ***names, size_t *count, int type_filter);

/**
 * @brief 执行插件（调用插件导出的 plugin_execute 符号）
 *
 * 技能类插件通过 plugin_execute 提供 JSON 入参 → JSON 出参的执行入口，
 * 与设计文档 airy_sys_skill_execute() 的契约一致。plugin_d 通过 dlsym
 * 解析可选符号 plugin_execute，未导出该符号的插件执行将返回
 * AIRY_ERR_NOT_FOUND。
 *
 * @param name       插件名称
 * @param json_input 输入 JSON 字符串
 * @param json_output 输出 JSON 字符串（由插件分配，调用者负责 AIRY_FREE）
 * @return 0 成功，非0失败
 */
int plugin_service_execute(const char *name, const char *json_input, char **json_output);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_PLUGIN_D_PLUGIN_SERVICE_H */
