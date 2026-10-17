// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tool_avail.c
 * @brief 工具可用性视图实现（0.1.19 D8）：声明一次、探测一次、到账一次。
 *
 * 语义与并发模型见 tool_avail.h。此处仅记两条工程纪律：
 *   1. 出网一律在锁外完成（仿 llm_d ep_health）：锁内取目标快照 →
 *      锁外 connect 探测 → 锁内按名回填，绝不持锁做 I/O。
 *   2. 依赖目标只存 socket 文件名：airy_runtime_dir_socket() 返回线程
 *      局部缓冲，指针跨线程不可留存；探测时按名即时取路径并立即拷入
 *      栈上 sockaddr_un。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "platform_paths.h"
#include "svc_logger.h"
#include "core/tool_avail.h"

#include <string.h>

#ifndef _WIN32
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define TOOL_AVAIL_MAX_SOCK 8
#define TOOL_AVAIL_MAX_LINK 32
#define TOOL_AVAIL_SOCK_LEN 64
#define TOOL_AVAIL_ID_LEN 64

/* up：-1 尚未探测（unknown）/ 0 不可达 / 1 可达。unknown 与可达同为
 * available（fail-open），仅"已知不可达"才摘除。 */
typedef struct {
    char name[TOOL_AVAIL_SOCK_LEN];
    int up;
} tool_sock_t;

typedef struct {
    char tool[TOOL_AVAIL_ID_LEN];
    char name[TOOL_AVAIL_SOCK_LEN];
} tool_link_t;

struct tool_avail {
    airy_mtx_t lock;
    size_t sock_n;
    tool_sock_t socks[TOOL_AVAIL_MAX_SOCK];
    size_t link_n;
    tool_link_t links[TOOL_AVAIL_MAX_LINK];
};

/* 持锁调用：返回 name 在 socks 中的下标，未登记返回 (size_t)-1。 */
static size_t sock_find(tool_avail_t *ta, const char *name)
{
    for (size_t i = 0; i < ta->sock_n; ++i) {
        if (strcmp(ta->socks[i].name, name) == 0)
            return i;
    }
    return (size_t)-1;
}

/* 单发可达探测：AF_UNIX connect 成功即可达。返回 1/0/-1（unknown）。
 * Windows 无法以 AF_UNIX 探测（maths_d 在 Windows 走 TCP），返回 unknown
 * 以保留既有暴露行为。 */
static int sock_probe(const char *name)
{
#ifdef _WIN32
    (void)name;
    return -1;
#else
    const char *path = airy_runtime_dir_socket(name);
    if (!path || !path[0])
        return -1;

    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
        return 0;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return rc == 0 ? 1 : 0;
#endif
}

tool_avail_t *tool_avail_new(void)
{
    tool_avail_t *ta = AIRY_CALLOC(1, sizeof(tool_avail_t));
    if (!ta)
        return NULL;
    airy_mtx_init(&ta->lock);
    return ta;
}

void tool_avail_free(tool_avail_t *ta)
{
    if (!ta)
        return;
    airy_mtx_destroy(&ta->lock);
    AIRY_FREE(ta);
}

int tool_avail_dep(tool_avail_t *ta, const char *tool_id, const char *sock_name)
{
    if (!ta || !tool_id || !sock_name || !tool_id[0] || !sock_name[0])
        return AIRY_ERR_INVALID_PARAM;
    if (strlen(tool_id) >= TOOL_AVAIL_ID_LEN || strlen(sock_name) >= TOOL_AVAIL_SOCK_LEN) {
        SVC_LOG_WARN("C-L06: TOOL-AVAIL: dep name too long: %s -> %s", tool_id, sock_name);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&ta->lock);
    if (ta->link_n >= TOOL_AVAIL_MAX_LINK) {
        airy_mtx_unlock(&ta->lock);
        return AIRY_ERR_STATE_ERROR;
    }
    if (sock_find(ta, sock_name) == (size_t)-1) {
        if (ta->sock_n >= TOOL_AVAIL_MAX_SOCK) {
            airy_mtx_unlock(&ta->lock);
            return AIRY_ERR_STATE_ERROR;
        }
        size_t si = ta->sock_n++;
        AIRY_STRNCPY_TERM(ta->socks[si].name, sock_name, TOOL_AVAIL_SOCK_LEN);
        ta->socks[si].up = -1;
    }
    AIRY_STRNCPY_TERM(ta->links[ta->link_n].tool, tool_id, TOOL_AVAIL_ID_LEN);
    AIRY_STRNCPY_TERM(ta->links[ta->link_n].name, sock_name, TOOL_AVAIL_SOCK_LEN);
    ta->link_n++;
    airy_mtx_unlock(&ta->lock);
    return 0;
}

void tool_avail_poll(tool_avail_t *ta)
{
    if (!ta)
        return;

    char snap[TOOL_AVAIL_MAX_SOCK][TOOL_AVAIL_SOCK_LEN];
    size_t n = 0;
    airy_mtx_lock(&ta->lock);
    for (size_t i = 0; i < ta->sock_n && n < TOOL_AVAIL_MAX_SOCK; ++i) {
        AIRY_STRNCPY_TERM(snap[n], ta->socks[i].name, TOOL_AVAIL_SOCK_LEN);
        n++;
    }
    airy_mtx_unlock(&ta->lock);

    for (size_t i = 0; i < n; ++i) {
        int up = sock_probe(snap[i]);
        if (up < 0)
            continue; /* unknown：保留既有结论，避免瞬时 socket() 失败误判 */
        int changed = 0;
        airy_mtx_lock(&ta->lock);
        size_t si = sock_find(ta, snap[i]);
        if (si != (size_t)-1 && ta->socks[si].up != up) {
            ta->socks[si].up = up;
            changed = 1;
        }
        airy_mtx_unlock(&ta->lock);
        if (changed)
            SVC_LOG_INFO("C-L06: TOOL-AVAIL: %s -> %s", snap[i], up ? "up" : "down");
    }
}

int tool_avail_ok(tool_avail_t *ta, const char *tool_id)
{
    if (!ta || !tool_id)
        return 1;

    int ok = 1;
    airy_mtx_lock(&ta->lock);
    for (size_t i = 0; i < ta->link_n; ++i) {
        if (strcmp(ta->links[i].tool, tool_id) != 0)
            continue;
        size_t si = sock_find(ta, ta->links[i].name);
        if (si != (size_t)-1 && ta->socks[si].up == 0)
            ok = 0;
        break;
    }
    airy_mtx_unlock(&ta->lock);
    return ok;
}
