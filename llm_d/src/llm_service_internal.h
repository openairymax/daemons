/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_service_internal.h
 * @brief Internal declarations shared across the LLM service files.
 */

#ifndef AIRY_RT_LLM_SERVICE_SPLIT_INTERNAL_H
#define AIRY_RT_LLM_SERVICE_SPLIT_INTERNAL_H

#include "service.h"
#include "cost_tracker.h"
#include "providers/registry.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Config-loading domain (service_config.c) ---- */

int ends_with(const char *str, const char *suffix);
pricing_rule_t *load_pricing_rules(cJSON *root, int *count);
void free_pricing_rules(pricing_rule_t *rules, int count);
int load_pricing_rules_from_yaml(const char *config_path, pricing_rule_t **out_rules,
                                 int *out_count);
int svc_load_model_config(const char *config_path, provider_config_t **out_providers,
                          size_t *out_count);
int svc_load_model_config_json(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count);

/* ---- Provider-management domain (service_providers.c) ---- */

void free_provider_configs(provider_config_t *providers, size_t count);
void merge_provider_configs(const provider_config_t *main_provs, size_t main_cnt,
                            const provider_config_t *user_provs, size_t user_cnt,
                            provider_config_t **out, size_t *out_cnt);
void register_router_endpoints(llm_service_t *svc);

/* ---- Complexity-evaluation and statistics domain (service_metrics.c) ---- */

/**
 * @brief Complexity assessment levels (BAN-133 coding contract)
 */
typedef enum {
    LLM_COMPLEXITY_SIMPLE = 0,
    LLM_COMPLEXITY_MODERATE = 1,
    LLM_COMPLEXITY_COMPLEX = 2
} llm_complexity_level_t;

llm_complexity_level_t assess_complexity(const char *input);
void log_routing_decision(const char *model, llm_complexity_level_t complexity,
                          size_t input_len, const char *reason);

/* ---- Daemon application domain (main.c split, 2026-08-27) ----
 *
 * Shared constants/types/globals between main.c (entry & wiring),
 * llm_daemon_request.c (params parsing), llm_daemon_methods.c (RPC methods)
 * and llm_daemon_config.c (daemon config assembly). The daemon_main.h
 * generated boilerplate (g_running_llm_d etc.) stays static inside main.c —
 * only symbols that cross those four files are declared here. */

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("llm.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_llm"
#define DEFAULT_TCP_PORT 8080
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64
#define MAX_THREADS 8
#define MAX_MESSAGES_PER_REQUEST 128

extern llm_service_t *g_service;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_threads;
    int max_clients;
} llm_daemon_config_t;

extern llm_daemon_config_t g_config;

typedef struct {
    llm_message_t messages[MAX_MESSAGES_PER_REQUEST];
    size_t message_count;
    char *response_buffer;
    size_t response_size;
    size_t response_capacity;
    char *tools_json;
} request_context_t;

/* llm_daemon_request.c */
request_context_t *request_context_create(void);
void request_context_destroy(request_context_t *ctx);
int parse_params(cJSON *params, request_context_t *ctx, llm_request_config_t *cfg);

/* llm_daemon_config.c */
int load_daemon_config(const char *config_path);
void free_daemon_config(void);
void destroy_service_llm_d(void);

/* llm_daemon_methods.c: JSON-RPC method adapters registered on the
 * dispatcher by main() */
void on_complete_method(cJSON *params, int id, void *user_data);
void on_complete_stream_method(cJSON *params, int id, void *user_data);
void on_list_models_method(cJSON *params, int id, void *user_data);
void on_embeddings_method(cJSON *params, int id, void *user_data);
void on_count_tokens_method(cJSON *params, int id, void *user_data);
void on_health_check_method(cJSON *params, int id, void *user_data);
void on_get_stats_method(cJSON *params, int id, void *user_data);

#ifdef HAVE_YAML
#include <yaml.h>

/* ---- YAML config-parsing infrastructure (service_config_yaml*.c split,
 *      2026-08-27): flat key/value map + parse state shared by the global-
 *      section loader, the models state machine, provider aggregation and
 *      pricing-rule extraction. ---- */

typedef struct {
    char key[128];
    char value[512];
} yaml_kv_t;

typedef struct {
    yaml_kv_t *pairs;
    size_t count;
    size_t capacity;
} yaml_map_t;

void yaml_map_init(yaml_map_t *m);
void yaml_map_add(yaml_map_t *m, const char *key, const char *value);
const char *yaml_map_get(const yaml_map_t *m, const char *key);
void yaml_map_free(yaml_map_t *m);

typedef struct {
    char name[128];
    char provider[64];
    char api_key_env[128];
    char endpoint[512];
    int timeout_sec;
    int max_retries;
    /* 2.1.1.5 修复：模型单价（model.yaml models[].input/output_cost_per_1k），
     * 用于生成 cost_tracker 的 pricing rules——此前 YAML 配置完全不加载
     * 价格，计费全部落到默认价 0.001/0.002，金额不真实。 */
    double input_cost_per_k;
    double output_cost_per_k;
    /* 价格字段是否在 YAML 中显式声明（区分"免费模型：显式 0"与
     * "未配置价格：字段缺失"）。免费模型（llama 等）显式 0.0 必须保留
     * 为零价规则，未配置的模型落默认价。 */
    int has_input_price;
    int has_output_price;
    /* v2 表格格式扩展字段（2026-08-26）：
     * mode / api_format / context_window / max_output / tool_rounds /
     * vision / thinking 来自模型连接表的每行配置。 */
    char mode[8];
    char api_format[16];
    char context_window[16];
    char max_output[16];
    int tool_rounds;
    int vision;
    char thinking[8];
} model_entry_t;

typedef struct {
    char name[64];
    char api_key_env[128];
    char base_url[512];
    int timeout_sec;
    int max_retries;
    char *model_names[64];
    size_t model_count;
} prov_cfg_t;

typedef struct {
    char name[64];
    char api_key_env[128];
    char base_url[512];
    int timeout_sec;
    int max_retries;
    char *model_names[64];
    size_t model_count;
} provider_agg_t;

typedef struct {
    yaml_map_t item_map;
    yaml_map_t prov_map;
    prov_cfg_t cur_p;
    prov_cfg_t pcfg[16];
    size_t pcfg_count;
    model_entry_t models[64];
    size_t model_count;
    int map_depth;
    int seq_depth;
    int in_models;
    int in_providers;
    int item_depth;
    int nested;
    char pending_key[128];
    int has_pending_key;
} svc_yaml_state_t;

/* service_config_yaml_models.c */
void svc_yaml_event_loop(yaml_parser_t *parser, svc_yaml_state_t *st, int *done);
void svc_yaml_expand_llm(svc_yaml_state_t *st, const char *config_path);

/* service_config_yaml.c / service_config_yaml_providers.c */
int svc_config_load_yaml(const char *config_path, service_config_t *cfg);
int svc_load_model_config_yaml(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count);
#endif /* HAVE_YAML */

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_SERVICE_SPLIT_INTERNAL_H */
