// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file example_svc_usage.c
 * @brief 服务管理框架使用示例
 *
 * 展示如何使用 svc_common.h 中定义的服务管理框架 API。
 * 此示例演示了服务的完整生命周期管理。
 *
 * 编译命令（Linux/macOS）:
 *   gcc -o example_svc_usage example_svc_usage.c -I./common/include -L./build/daemons/common
 * -lsvc_common -lairy_common -lpthread
 *
 * 编译命令（Windows）:
 *   cl example_svc_usage.c /I./common/include /link svc_common.lib airy_common.lib
 */

#include "svc_common.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 示例服务上下文
 */
typedef struct {
    int counter;
    char name[64];
} example_service_context_t;

/**
 * @brief 示例服务初始化函数
 */
static airy_err_t example_service_init(airy_svc_t svc, const airy_svc_config_t *config)
{

    printf("示例服务初始化: %s\n", airy_svc_get_name(svc));

    example_service_context_t *ctx =
        (example_service_context_t *)AIRY_MALLOC(sizeof(example_service_context_t));
    if (!ctx) {
        return AIRY_ENOMEM;
    }

    __builtin_memset(ctx, 0, sizeof(example_service_context_t));
    AIRY_STRNCPY_TERM(ctx->name, config->name, sizeof(ctx->name));
    ctx->counter = 0;

    return AIRY_SUCCESS;
}

/**
 * @brief 示例服务启动函数
 */
static airy_err_t example_service_start(airy_svc_t svc)
{
    printf("示例服务启动: %s\n", airy_svc_get_name(svc));
    return AIRY_SUCCESS;
}

/**
 * @brief 示例服务停止函数
 */
static airy_err_t example_service_stop(airy_svc_t svc, bool force)
{

    printf("示例服务停止: %s (强制: %s)\n", airy_svc_get_name(svc), force ? "是" : "否");
    return AIRY_SUCCESS;
}

/**
 * @brief 示例服务销毁函数
 */
static void example_service_destroy(airy_svc_t svc)
{
    printf("示例服务销毁: %s\n", airy_svc_get_name(svc));
}

/**
 * @brief 示例服务健康检查函数
 */
static airy_err_t example_service_healthcheck(airy_svc_t svc)
{
    printf("示例服务健康检查: %s\n", airy_svc_get_name(svc));
    return AIRY_SUCCESS;
}

/**
 * @brief 创建示例服务接口
 */
static airy_svc_interface_t create_example_service_interface(void)
{
    airy_svc_interface_t iface = {.init = example_service_init,
                                  .start = example_service_start,
                                  .stop = example_service_stop,
                                  .destroy = example_service_destroy,
                                  .healthcheck = example_service_healthcheck};
    return iface;
}

int main(void)
{
    printf("=== AgentRT 服务管理框架使用示例 ===\n\n");

    airy_err_t err = AIRY_SUCCESS;
    airy_svc_t service = NULL;

    airy_svc_config_t config = {.name = "example-service",
                                .version = "0.1.1",
                                .capabilities = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_PAUSEABLE,
                                .max_concurrent = 10,
                                .timeout_ms = 5000,
                                .priority = 5,
                                .auto_start = true,
                                .enable_metrics = true,
                                .enable_tracing = false};

    airy_svc_interface_t iface = create_example_service_interface();

    printf("1. 创建服务...\n");
    err = airy_svc_create(&service, "example-service", &iface, &config);
    if (err != AIRY_SUCCESS) {
        printf("  创建服务失败: %d\n", err);
        return 1;
    }
    printf("  服务创建成功: %s\n", airy_svc_get_name(service));

    printf("\n2. 初始化服务...\n");
    err = airy_svc_init(service);
    if (err != AIRY_SUCCESS) {
        printf("  服务初始化失败: %d\n", err);
        airy_svc_destroy(service);
        return 1;
    }
    printf("  服务初始化成功，状态: %s\n", airy_svc_state_to_string(airy_svc_get_state(service)));

    printf("\n3. 启动服务...\n");
    err = airy_svc_start(service);
    if (err != AIRY_SUCCESS) {
        printf("  服务启动失败: %d\n", err);
        airy_svc_destroy(service);
        return 1;
    }
    printf("  服务启动成功，状态: %s\n", airy_svc_state_to_string(airy_svc_get_state(service)));

    printf("\n4. 检查服务状态...\n");
    printf("  服务名称: %s\n", airy_svc_get_name(service));
    printf("  服务版本: %s\n", airy_svc_get_version(service));
    printf("  服务状态: %s\n", airy_svc_state_to_string(airy_svc_get_state(service)));
    printf("  是否就绪: %s\n", airy_svc_is_ready(service) ? "是" : "否");
    printf("  是否运行: %s\n", airy_svc_is_running(service) ? "是" : "否");

    printf("\n5. 执行健康检查...\n");
    err = airy_svc_healthcheck(service);
    if (err != AIRY_SUCCESS) {
        printf("  健康检查失败: %d\n", err);
    } else {
        printf("  健康检查通过\n");
    }

    printf("\n6. 获取服务统计...\n");
    airy_svc_stats_t stats;
    err = airy_svc_get_stats(service, &stats);
    if (err != AIRY_SUCCESS) {
        printf("  获取统计失败: %d\n", err);
    } else {
        printf("  请求总数: %llu\n", (unsigned long long)stats.request_count);
        printf("  成功次数: %llu\n", (unsigned long long)stats.success_count);
        printf("  错误次数: %llu\n", (unsigned long long)stats.error_count);
    }

    printf("\n7. 测试暂停/恢复功能...\n");
    if (airy_svc_has_capability(service, AIRY_SVC_CAP_PAUSEABLE)) {
        printf("  服务支持暂停功能\n");

        err = airy_svc_pause(service);
        if (err != AIRY_SUCCESS) {
            printf("  暂停失败: %d\n", err);
        } else {
            printf("  服务已暂停，状态: %s\n",
                   airy_svc_state_to_string(airy_svc_get_state(service)));

            err = airy_svc_resume(service);
            if (err != AIRY_SUCCESS) {
                printf("  恢复失败: %d\n", err);
            } else {
                printf("  服务已恢复，状态: %s\n",
                       airy_svc_state_to_string(airy_svc_get_state(service)));
            }
        }
    } else {
        printf("  服务不支持暂停功能\n");
    }

    printf("\n8. 停止服务...\n");
    err = airy_svc_stop(service, false);
    if (err != AIRY_SUCCESS) {
        printf("  服务停止失败: %d，尝试强制停止...\n", err);
        err = airy_svc_stop(service, true);
        if (err != AIRY_SUCCESS) {
            printf("  强制停止也失败: %d\n", err);
        }
    }
    printf("  服务停止成功，状态: %s\n", airy_svc_state_to_string(airy_svc_get_state(service)));

    printf("\n9. 销毁服务...\n");
    airy_svc_destroy(service);
    printf("  服务销毁完成\n");

    printf("\n10. 注册表统计...\n");
    uint32_t service_count = airy_svc_count();
    printf("  注册表中的服务数量: %u\n", service_count);

    printf("\n=== 示例程序完成 ===\n");

    return 0;
}
