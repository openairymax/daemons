// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gw_cli.c
 * @brief gateway_d CLI 域：usage 与命令行参数解析（装配分层归 main.c）。
 */

#include "gateway_d_internal.h"

#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>    /* open /dev/null (daemonize) */
#include <sys/stat.h> /* umask */
#include <unistd.h>
#endif

static void print_usage(const char *prog)
{
    char buf[256];
    fputs("AgentRT Gateway Daemon\n", stdout);
    snprintf(buf, sizeof(buf), "Usage: %s [options]\n\n", prog);
    fputs(buf, stdout);
    fputs("Options:\n", stdout);
    fputs("  -c <config>   Configuration file path\n", stdout);
    fputs("  -h <host>     HTTP gateway host (default: 0.0.0.0)\n", stdout);
    fputs("  -p <port>     HTTP gateway port (default: 8080)\n", stdout);
    fputs("  -w <port>     WebSocket gateway port (default: 8081)\n", stdout);
    fputs("  -s            Enable stdio gateway\n", stdout);
    fputs("  -d            Run as daemon (Unix only)\n", stdout);
    fputs("  -v            Enable metrics reporting (default: on)\n", stdout);
    fputs("  --help        Show this help\n", stdout);
    fputs("\nExamples:\n", stdout);
    snprintf(buf, sizeof(buf), "  %s -h 127.0.0.1 -p 8080\n", prog);
    fputs(buf, stdout);
    snprintf(buf, sizeof(buf), "  %s -c AIRY_CONFIG_DIR \"/gateway.conf\"\n", prog);
    fputs(buf, stdout);
}

int gw_parse_args(int argc, char *argv[], gateway_service_config_t *config,
                  bool *daemonize)
{
    gateway_service_get_default_config(config);
    if (daemonize)
        *daemonize = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--manager") == 0 && i + 1 < argc) {
            /* 兼容 bootstrap 统一 daemon 启动参数：--manager 指向 agentrt.yaml
             * 全局配置（bootstrap 对全部 daemon 一致传参）。gateway_d 细粒度
             * 配置仍以默认值 + 环境变量为准（load_config 仅解析 key=value，
             * 不解析 YAML；缺失时保持默认 8080/8081，与 agentrt.yaml 一致）。 */
            i++;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            airy_err_t err = gateway_service_load_config(config, argv[++i]);
            if (err != AIRY_SUCCESS) {
                SVC_LOG_ERROR("Failed to load config: %s", argv[i]);
                AIRY_ERROR(AIRY_ERR_IO, "failed to load config file");
            }
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            i++; /* 显式消费选项参数，不依赖宏求值次数 */
            AIRY_STRNCPY_TERM(config->http.host, argv[i], GATEWAY_HOST_MAX);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config->http.port = (uint16_t)strtol(argv[++i], NULL, 10);
            config->http.enabled = true;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            config->ws.port = (uint16_t)strtol(argv[++i], NULL, 10);
            config->ws.enabled = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            config->stdio.enabled = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            config->enable_metrics = true;
        } else if (strcmp(argv[i], "-d") == 0) {
#ifndef _WIN32
            /* 仅记录意图；实际守护化由 gw_daemonize() 在 main 中、任何线程
             * 创建之前执行（解析与动作分离，规避多线程 fork 继承锁）。 */
            if (daemonize)
                *daemonize = true;
#else
            SVC_LOG_WARN("-d not supported on Windows");
#endif
        } else {
            SVC_LOG_ERROR("Unknown option: %s", argv[i]);
            AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "unknown option");
        }
    }
    return 0;
}

#ifndef _WIN32
int gw_daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        SVC_LOG_ERROR("Failed to fork");
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "fork failed when daemonizing");
    }
    if (pid > 0)
        exit(0); /* 父进程退出，子进程继续守护运行 */

    if (setsid() < 0)
        SVC_LOG_WARN("setsid() failed during daemonize");
    umask(022);
    if (chdir("/") != 0)
        SVC_LOG_ERROR("chdir(\"/\") failed during daemonize");
    /* 标准流重定向到 /dev/null 而非 fclose：fclose 后 stdio/日志再写已关闭
     * 的流是 UB（fd 可能被后续 socket 复用） */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        if (dup2(devnull, STDIN_FILENO) < 0 || dup2(devnull, STDOUT_FILENO) < 0 ||
            dup2(devnull, STDERR_FILENO) < 0) {
            close(devnull);
            AIRY_ERROR(AIRY_ERR_IO, "dup2 /dev/null failed when daemonizing");
        }
        if (devnull > STDERR_FILENO)
            close(devnull);
    }
    return 0;
}
#endif /* !_WIN32 */
