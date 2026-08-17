// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/* _GNU_SOURCE: defined via CMakeLists.txt target_compile_definitions (BAN-182) */
#include "atomic_compat.h"
#include "daemon_platform_ext.h"
#include "airy_memory.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "error.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <errno.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <poll.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

#include <cjson/cJSON.h>

#ifndef AIRY_TIMESTAMP_T_DEFINED
typedef uint64_t airy_timestamp_t;
#endif

#ifndef AIRY_DL_T_DEFINED
typedef void *airy_dl_t;
#define AIRY_DL_T_DEFINED
#endif

int airy_time_now(airy_timestamp_t *ts)
{
    if (!ts)
        return AIRY_ERR_INVALID_PARAM;
    *ts = airy_time_ns();
    return 0;
}

int airy_time_monotonic(airy_timestamp_t *ts)
{
    return airy_time_now(ts);
}

uint64_t airy_time_to_ms(const airy_timestamp_t *ts)
{
    if (!ts)
        return 0;
    return *ts / 1000000ULL;
}

void airy_time_from_ms(uint64_t ms, airy_timestamp_t *ts)
{
    if (!ts)
        return;
    *ts = ms * 1000000ULL;
}

uint32_t airy_process_self(void)
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

uint64_t airy_thread_self(void)
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

int airy_thread_setname(const char *name)
{
#ifdef __linux__
    return pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    return pthread_setname_np(name);
#else

    if (name && name[0]) {
    }
    return 0;
#endif
}

int airy_thread_getname(char *name, size_t size)
{
#ifdef __linux__
    if (pthread_getname_np(pthread_self(), name, size) != 0)
        name[0] = '\0';
    return 0;
#elif defined(__APPLE__)
    if (size > 0)
        name[0] = '\0';
    return 0;
#else

    if (name && size > 0)
        name[0] = '\0';
    return AIRY_ERR_UNKNOWN;
#endif
}

int airy_mkdir(const char *path, int recursive)
{
    if (!path)
        return AIRY_ERR_INVALID_PARAM;
#ifdef _WIN32

    if (recursive > 0) {
    }
    return _mkdir(path);
#else
    if (recursive) {
        char tmp[PATH_MAX];
        size_t len = strlen(path);
        if (len == 0 || len >= PATH_MAX)
            return AIRY_ERR_OVERFLOW;
        __builtin_memcpy(tmp, path, len + 1);
        for (size_t i = (tmp[0] == '/') ? 1 : 0; i <= len; i++) {
            if (tmp[i] == '/' || tmp[i] == '\0') {
                char saved = tmp[i];
                tmp[i] = '\0';
                if (i > 0 && tmp[0] != '\0') {
                    struct stat st;
                    if (stat(tmp, &st) != 0) {
                        if (mkdir(tmp, 0755) != 0) {
                            tmp[i] = saved;
                            return AIRY_ERR_IO;
                        }
                    }
                }
                tmp[i] = saved;
            }
        }
        return 0;
    }
    return mkdir(path, 0755);
#endif
}

airy_dl_t airy_dl_open(const char *path)
{
#ifdef _WIN32
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW);
#endif
}

int airy_dl_close(airy_dl_t dl)
{
#ifdef _WIN32
    return FreeLibrary(dl) ? 0 : AIRY_ERR_UNKNOWN;
#else
    return dlclose(dl);
#endif
}

void *airy_dl_sym(airy_dl_t dl, const char *symbol)
{
#ifdef _WIN32
    return (void *)GetProcAddress(dl, symbol);
#else
    return dlsym(dl, symbol);
#endif
}

const char *airy_dl_error(void)
{
#ifdef _WIN32
    static char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), 0, buf, sizeof(buf), NULL);
    return buf;
#else
    return dlerror();
#endif
}

int airy_get_sysinfo(airy_sysinfo_t *info)
{
    if (!info)
        return AIRY_ERR_INVALID_PARAM;
    __builtin_memset(info, 0, sizeof(*info));
#ifdef __linux__
    {
        struct sysinfo si;
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (sysinfo(&si) != 0)
            return AIRY_ERR_UNKNOWN;
        AIRY_STRNCPY_TERM(info->os_name, "Linux", sizeof(info->os_name));
        info->cpu_count = ncpu > 0 ? (uint32_t)ncpu : 1;
        info->memory_total = (uint64_t)si.totalram * si.mem_unit;
        info->memory_free = (uint64_t)si.freeram * si.mem_unit;
        gethostname(info->hostname, sizeof(info->hostname));
    }
    return 0;
#elif defined(__APPLE__)
    AIRY_STRNCPY_TERM(info->os_name, "macOS", sizeof(info->os_name));
    {
        int mib[2];
        size_t len;
        int64_t memsize = 0;

        mib[0] = CTL_HW;
        mib[1] = HW_MEMSIZE;
        len = sizeof(memsize);
        if (sysctl(mib, 2, &memsize, &len, NULL, 0) == 0)
            info->memory_total = (uint64_t)memsize;

        mib[0] = CTL_HW;
        mib[1] = HW_NCPU;
        int ncpu = 0;
        len = sizeof(ncpu);
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0)
            info->cpu_count = (uint32_t)ncpu;

        /* HW_MEMSIZE returns physical total; free memory requires
         * host_statistics64 which is not available in all SDKs, so
         * approximate free as total (conservative for parallelism
         * decisions — over-estimation is safe). */
        info->memory_free = info->memory_total;
    }
    gethostname(info->hostname, sizeof(info->hostname));
    return 0;
#elif defined(_WIN32)
    AIRY_STRNCPY_TERM(info->os_name, "Windows", sizeof(info->os_name));
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        info->cpu_count = (uint32_t)si.dwNumberOfProcessors;

        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            info->memory_total = (uint64_t)ms.ullTotalPhys;
            info->memory_free = (uint64_t)ms.ullAvailPhys;
        }
    }
    {
        DWORD sz = sizeof(info->hostname);
        GetComputerNameA(info->hostname, &sz);
    }
    return 0;
#else
    return 0;
#endif
}

#include <errno.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
/* macOS/BSD compatibility: SOCK_NONBLOCK / MSG_NOSIGNAL / MSG_DONTWAIT are
   Linux-specific flags; defined as 0 on non-Linux POSIX platforms; after
   socket creation, fcntl / SO_NOSIGPIPE are used instead. */
#ifndef __linux__
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#endif
#else

#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif
#ifndef _WS2TCPIP_H_
#include <ws2tcpip.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#endif

static atomic_int g_socket_initialized = 0;

int airy_sock_init(void)
{
    int expected = 0;
    atomic_compare_exchange_strong_explicit(&g_socket_initialized, &expected, 1,
                                            memory_order_seq_cst, memory_order_seq_cst);
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        return AIRY_ERR_IO;
#endif
    return 0;
}

void airy_sock_cleanup(void)
{
    atomic_store_explicit(&g_socket_initialized, 0, memory_order_seq_cst);
#ifdef _WIN32
    WSACleanup();
#endif
}

airy_sock_t airy_sock_create_tcp_server(const char *host, uint16_t port)
{
    if (!host)
        return AIRY_ERR_INVALID_PARAM;

#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET)
        return AIRY_INVALID_SOCKET;
    {
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
    }
#else
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (fd < 0)
        return AIRY_ERR_IO;
#ifndef __linux__

    {
        int nb_flags = fcntl(fd, F_GETFL, 0);
        if (nb_flags >= 0)
            fcntl(fd, F_SETFL, nb_flags | O_NONBLOCK);
    }
#endif
#ifdef SO_NOSIGPIPE

    {
        int nosig_on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&nosig_on, sizeof(nosig_on));
    }
#endif
#endif

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(fd);
        return AIRY_INVALID_SOCKET;
#else
        close(fd);
        return AIRY_ERR_IO;
#endif
    }

    if (listen(fd, SOMAXCONN) != 0) {
#ifdef _WIN32
        closesocket(fd);
        return AIRY_INVALID_SOCKET;
#else
        close(fd);
        return AIRY_ERR_IO;
#endif
    }

    return fd;
}

#if AIRY_PLATFORM_POSIX
airy_sock_t airy_sock_create_unix_server(const char *path)
{
    if (!path)
        return AIRY_ERR_INVALID_PARAM;

    {
        char dir_buf[256];
        size_t len = strlen(path);
        if (len >= sizeof(dir_buf))
            return AIRY_ERR_INVALID_PARAM;
        AIRY_MEMCPY(dir_buf, path, len + 1);
        char *slash = strrchr(dir_buf, '/');
        if (slash) {
            *slash = '\0';
            airy_mkdir_p(dir_buf);
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return AIRY_ERR_IO;
#ifndef __linux__

    {
        int nb_flags = fcntl(fd, F_GETFL, 0);
        if (nb_flags >= 0)
            fcntl(fd, F_SETFL, nb_flags | O_NONBLOCK);
    }
#endif
#ifdef SO_NOSIGPIPE

    {
        int nosig_on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&nosig_on, sizeof(nosig_on));
    }
#endif

    struct sockaddr_un addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, path, sizeof(addr.sun_path));

    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return AIRY_ERR_IO;
    }

    if (listen(fd, SOMAXCONN) != 0) {
        close(fd);
        return AIRY_ERR_IO;
    }

    return fd;
}
#endif

airy_sock_t airy_sock_accept(airy_sock_t server_fd, uint32_t timeout_ms)
{
#ifdef _WIN32
    if (server_fd == AIRY_INVALID_SOCKET)
        return AIRY_ERR_INVALID_PARAM;

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(server_fd, &read_set);

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout_ms > 0) {
        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
        ptv = &tv;
    }

    int ret = select(0, &read_set, NULL, NULL, ptv);
    if (ret <= 0 || !FD_ISSET(server_fd, &read_set))
        return AIRY_INVALID_SOCKET;

    struct sockaddr_in client_addr;
    int client_len = sizeof(client_addr);
    SOCKET client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd != INVALID_SOCKET) {
        u_long mode = 1;
        ioctlsocket(client_fd, FIONBIO, &mode);
    }

    return client_fd;
#else
    if (server_fd < 0)
        return AIRY_ERR_INVALID_PARAM;

    struct pollfd pfd;
    pfd.fd = server_fd;
    pfd.events = POLLIN;

    /* timeout_ms==0 means a non-blocking probe (return immediately), not an
     * infinite block: historical defect — 0 was mapped to poll(-1) infinite
     * wait; when a signal (e.g. SIGTERM) arrived exactly while the
     * event-loop thread was in accept, poll returned EINTR and then re-entered
     * infinite blocking, so the loop could never check stop_requested and the
     * daemon could not exit gracefully. */
    int timeout = timeout_ms == 0 ? 0 : (int)timeout_ms;
    int ret;
    do {
        ret = poll(&pfd, 1, timeout);
    } while (ret < 0 && errno == EINTR);

    if (ret <= 0 || !(pfd.revents & POLLIN))
        return AIRY_ERR_IO;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd >= 0) {
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NOSIGPIPE

        {
            int nosig_on = 1;
            setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&nosig_on,
                       sizeof(nosig_on));
        }
#endif
    }

    return client_fd;
#endif
}

ssize_t airy_sock_recv(airy_sock_t sock, void *buf, size_t len)
{
#ifdef _WIN32
    if (sock == AIRY_INVALID_SOCKET || !buf || len == 0)
        return AIRY_ERR_INVALID_PARAM;
    ssize_t ret = recv(sock, buf, (int)len, 0);
    if (ret < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
        return 0;
    return ret;
#else
    if (sock < 0 || !buf || len == 0)
        return AIRY_ERR_INVALID_PARAM;
    ssize_t ret = recv(sock, buf, len, MSG_DONTWAIT);
    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return ret;
#endif
}

ssize_t airy_sock_send(airy_sock_t sock, const void *buf, size_t len)
{
#ifdef _WIN32
    if (sock == AIRY_INVALID_SOCKET || !buf || len == 0)
        return AIRY_ERR_INVALID_PARAM;
    ssize_t total_sent = 0;
    const uint8_t *ptr = (const uint8_t *)buf;

    while (total_sent < (ssize_t)len) {
        ssize_t sent = send(sock, ptr + total_sent, (int)(len - total_sent), 0);
        if (sent <= 0) {
            if (total_sent > 0)
                break;
            return sent;
        }
        total_sent += sent;
    }
    return total_sent;
#else
    if (sock < 0 || !buf || len == 0)
        return AIRY_ERR_INVALID_PARAM;
    ssize_t total_sent = 0;
    const uint8_t *ptr = (const uint8_t *)buf;

    while (total_sent < (ssize_t)len) {
        ssize_t sent = send(sock, ptr + total_sent, len - total_sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent <= 0) {
            if (total_sent > 0)
                break;
            return sent;
        }
        total_sent += sent;
    }
    return total_sent;
#endif
}

/* ---------------------------------------------------------------------------
 * Daemon request frame reading
 * ---------------------------------------------------------------------------
 *
 * Airymax daemons are "single-request-single-response-then-close": the
 * client sends one JSON-RPC request frame over a fresh connection and keeps
 * the socket open while waiting for the reply, so there is no EOF/close to
 * delimit the request. The historical daemon_main.h handler performed a
 * SINGLE airy_sock_recv() capped at MAX_BUFFER (64 KiB): any request that
 * grew past 64 KiB was truncated, failed the JSON parse and was rejected.
 * Multi-round tool loops hit this in practice -- llm.complete after feeding
 * back web_fetch results carries tens/hundreds of KiB of HTML, so the final
 * tool-loop round was silently killed (daily chat showed tool cards but
 * never streamed the final answer).
 *
 * airy_daemon_read_request() mirrors the client-side rpc_recv_response()
 * loop (daemon_rpc_client.c): poll in slices, recv what is available, probe
 * cJSON_Parse for a complete frame, grow the buffer up to
 * AIRY_DAEMON_REQ_MAX_CAP (8 MiB). A parseable frame ends the request; a
 * read budget (5 s) guards against stalled peers. On success the caller owns
 * the returned AIRY_MALLOC buffer (*out_len set); on failure NULL is
 * returned and *err (if non-NULL) points to a static reason string.
 */
#define AIRY_DAEMON_REQ_INITIAL_CAP (64 * 1024)
#define AIRY_DAEMON_REQ_MAX_CAP (8 * 1024 * 1024)
#define AIRY_DAEMON_REQ_FIRST_POLL_MS 5000
#define AIRY_DAEMON_REQ_SLICE_MS 200

char *airy_daemon_read_request(airy_sock_t client_fd, size_t *out_len, const char **err)
{
    if (out_len)
        *out_len = 0;
    if (err)
        *err = NULL;
    if (client_fd < 0)
        return NULL;

#if AIRY_PLATFORM_POSIX
    /* Wait for the first byte (5 s). The fd accepted by the event driver may
     * not have data yet; airy_sock_recv is MSG_DONTWAIT, so an immediate recv
     * can return 0 on EAGAIN, be mistaken for a failed connection and closed
     * (SIGPIPE / lost request race). */
    {
        struct pollfd pfd;
        pfd.fd = (int)client_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, AIRY_DAEMON_REQ_FIRST_POLL_MS);
        if (pr <= 0 || !(pfd.revents & POLLIN)) {
            if (err)
                *err = "Request read timeout";
            return NULL;
        }
    }

    size_t cap = AIRY_DAEMON_REQ_INITIAL_CAP;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf) {
        if (err)
            *err = "Out of memory";
        return NULL;
    }
    size_t used = 0;
    uint32_t elapsed = 0;

    for (;;) {
        if (used >= cap - 1) {
            size_t new_cap = cap * 2;
            if (new_cap > AIRY_DAEMON_REQ_MAX_CAP)
                new_cap = AIRY_DAEMON_REQ_MAX_CAP;
            if (new_cap <= cap) {
                if (err)
                    *err = "Request too large";
                AIRY_FREE(buf);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(buf, new_cap);
            if (!np) {
                if (err)
                    *err = "Out of memory";
                AIRY_FREE(buf);
                return NULL;
            }
            buf = np;
            cap = new_cap;
        }

        ssize_t n = airy_sock_recv(client_fd, buf + used, cap - used - 1);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                /* no data at this instant: fall through to the poll below */
            } else {
                if (err)
                    *err = "Request read error";
                AIRY_FREE(buf);
                return NULL;
            }
        } else if (n > 0) {
            used += (size_t)n;
            buf[used] = '\0';
        }

        /* A parseable frame ends the request (the client keeps the connection
         * open while waiting for the reply, so there is no EOF to rely on). */
        if (used > 0) {
            cJSON *probe = cJSON_Parse(buf);
            if (probe) {
                cJSON_Delete(probe);
                break;
            }
        }

        if (elapsed >= AIRY_DAEMON_REQ_FIRST_POLL_MS) {
            /* Read budget exhausted without a complete frame. */
            if (err)
                *err = "Request read timeout";
            AIRY_FREE(buf);
            return NULL;
        }
        struct pollfd pfd;
        pfd.fd = (int)client_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        uint32_t remain = (AIRY_DAEMON_REQ_FIRST_POLL_MS - elapsed) < AIRY_DAEMON_REQ_SLICE_MS
                              ? (AIRY_DAEMON_REQ_FIRST_POLL_MS - elapsed)
                              : AIRY_DAEMON_REQ_SLICE_MS;
        int pr = poll(&pfd, 1, (int)remain);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            if (err)
                *err = "Request read error";
            AIRY_FREE(buf);
            return NULL;
        }
        if (pr == 0) {
            elapsed += remain;
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            /* Hangup before a complete frame. */
            if (err)
                *err = "Request read error";
            AIRY_FREE(buf);
            return NULL;
        }
        elapsed += 1;
    }

    *out_len = used;
    return buf;
#else
    /* Windows fallback：与 POSIX 主路径相同的循环读 + JSON 完整性探测，
     * 消除历史单次 recv 的 64 KiB 截断（llm.complete 携带大 tool 结果时
     * 会超限被截成 JSON 解析错误）。命名管道/TCP 均按完整帧语义读取。 */
    char *buf = (char *)AIRY_MALLOC(AIRY_DAEMON_REQ_INITIAL_CAP);
    if (!buf) {
        if (err)
            *err = "Out of memory";
        return NULL;
    }
    size_t cap = AIRY_DAEMON_REQ_INITIAL_CAP;
    size_t used = 0;
    uint32_t elapsed = 0;

    for (;;) {
        if (used >= cap - 1) {
            size_t new_cap = cap * 2;
            if (new_cap > AIRY_DAEMON_REQ_MAX_CAP)
                new_cap = AIRY_DAEMON_REQ_MAX_CAP;
            if (new_cap <= cap) {
                if (err)
                    *err = "Request too large";
                AIRY_FREE(buf);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(buf, new_cap);
            if (!np) {
                if (err)
                    *err = "Out of memory";
                AIRY_FREE(buf);
                return NULL;
            }
            buf = np;
            cap = new_cap;
        }

        ssize_t n = airy_sock_recv(client_fd, buf + used, cap - used - 1);
        if (n > 0) {
            used += (size_t)n;
            buf[used] = '\0';
            cJSON *probe = cJSON_Parse(buf);
            if (probe) {
                cJSON_Delete(probe);
                break;
            }
            continue;
        }

        if (elapsed >= AIRY_DAEMON_REQ_FIRST_POLL_MS) {
            if (err)
                *err = "Request read timeout";
            AIRY_FREE(buf);
            return NULL;
        }
        /* Windows 无 poll：以 50ms select 切片刻轮询（等价 POSIX 切片） */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET((SOCKET)client_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = AIRY_DAEMON_REQ_SLICE_MS * 1000;
        int pr = select(0, &rfds, NULL, NULL, &tv);
        if (pr < 0) {
            if (err)
                *err = "Request read error";
            AIRY_FREE(buf);
            return NULL;
        }
        if (pr == 0)
            elapsed += AIRY_DAEMON_REQ_SLICE_MS;
    }

    *out_len = used;
    return buf;
#endif
}
