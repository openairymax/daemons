// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file market_service_impl.c
 * @brief Market service core implementation.
 * @details Defines struct market_service and implements all public APIs in
 *          market_service.h.
 */

#include "daemon_errors.h"
#include "market_service.h"
#include "platform.h"
#include "svc_logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef _WIN32
#include <ftw.h>
#include <unistd.h>
#include <sys/wait.h>
#else
#include <windows.h>
#endif

#include <airymax/sched.h>

#include "market_service_internal.h"

#ifdef _WIN32
/**
 * @brief Windows equivalent of synchronous external command execution
 *        (replacing POSIX fork/execlp/waitpid).
 *
 * Semantics aligned with the original curl/tar calls:
 *   - No shell: lpApplicationName=NULL, the first command-line token is the
 *     executable name, CreateProcess searches PATH (equivalent to execlp),
 *     no command-injection risk;
 *   - Output inherits the parent's console handles (matching the POSIX
 *     version's unredirected behavior);
 *   - Blocks until the child exits and returns its exit code; CreateProcess
 *     failure returns -1 (equivalent to the non-zero semantics of POSIX fork
 *     failure / execlp failure _exit(127)).
 *
 * Limitation: Windows re-parses the command line into argv via CreateProcess;
 * arguments containing spaces are double-quote escaped; in this context the
 * arguments are controlled URL/path/flags, safe to handle.
 */
int win_run_command(const char *prog, const char *const args[])
{
    char cmdline[2048];
    size_t off = 0;
    int n = snprintf(cmdline, sizeof(cmdline), "\"%s\"", prog);
    if (n < 0)
        return AIRY_ERR_FAIL;
    off = (size_t)n;
    for (size_t i = 0; args && args[i] && off < sizeof(cmdline) - 1; i++) {
        const char *a = args[i];
        int quote = (strpbrk(a, " \t\"") != NULL);
        n = quote ? snprintf(cmdline + off, sizeof(cmdline) - off, " \"%s\"", a) :
                    snprintf(cmdline + off, sizeof(cmdline) - off, " %s", a);
        if (n < 0)
            break;
        off += (size_t)n;
    }
    cmdline[sizeof(cmdline) - 1] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return AIRY_ERR_EXEC_FAIL;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#endif /* _WIN32 */

#ifndef _WIN32
static int nftw_remove_cb(const char *fpath, const struct stat *sb, int typeflag,
                          struct FTW *ftwbuf)
{
    (void)sb;
    (void)ftwbuf;
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        return rmdir(fpath);
    }
    return remove(fpath);
}
#endif

/**
 * @brief Recursively remove a directory tree (cross-platform)
 *
 * POSIX: depth-first traversal via nftw.
 * Windows: recursive descent via FindFirstFile/FindNextFile, clearing the
 *          read-only attribute before deleting files, then RemoveDirectory
 *          for empty dirs. Behavior aligned with the POSIX version; no shell
 *          invocation, no command-injection risk.
 */
int recursive_remove(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return AIRY_ERR_NOT_FOUND;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {

        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) ? 0 : -1;
    }

    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryA(path) ? 0 : -1;
    }
    do {
        if (fd.cFileName[0] == '.')
            continue;
        char child[MAX_PATH];
        snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            recursive_remove(child);
        } else {
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    return nftw(path, nftw_remove_cb, 64, FTW_DEPTH | FTW_PHYS);
#endif
}

int is_safe_for_shell(const char *str)
{
    if (!str)
        return 0;
    const char *dangerous = ";|&$`'\"\\(){}[]!#~<>\n\r";
    for (size_t i = 0; i < strlen(dangerous); i++) {
        if (strchr(str, dangerous[i]))
            return 0;
    }
    return 1;
}

int is_valid_url(const char *url)
{
    if (!url || strlen(url) == 0)
        return 0;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return 0;
    return is_safe_for_shell(url);
}

int is_safe_path_component(const char *str)
{
    if (!str || strlen(str) == 0)
        return 0;
    if (strchr(str, '/') || strchr(str, '\\'))
        return 0;
    if (strstr(str, ".."))
        return 0;
    return 1;
}

/* Default storage root: $AIRY_HOME/agents (or /skills).
 * Historically install/uninstall hardcoded the "./agents" relative path,
 * which drifted with the process CWD and inevitably landed in the wrong place
 * after deployment to $AIRY_HOME/bin; unified into the AIRY_HOME path scheme. */
const char *market_default_storage(const char *subdir)
{
    static __thread char s_path[AIRY_PATH_MAX];
    snprintf(s_path, sizeof(s_path), "%s/%s", airy_home_dir(), subdir);
    return s_path;
}

int market_service_create(const market_config_t *config, market_service_t **service)
{
    market_config_t default_cfg;
    if (!service) {
        SVC_LOG_ERROR("market_service_create: NULL service output parameter");
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!config) {
        __builtin_memset(&default_cfg, 0, sizeof(default_cfg));
        default_cfg.cache_ttl_ms = 3600000;
        default_cfg.sync_interval_ms = 30000;
        config = &default_cfg;
    }

    market_service_t *svc = (market_service_t *)AIRY_CALLOC(1, sizeof(market_service_t));
    if (!svc) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate service struct");
    }

    __builtin_memcpy(&svc->config, config, sizeof(market_config_t));
    if (config->registry_url)
        svc->config.registry_url = AIRY_STRDUP(config->registry_url);
    if (config->storage_path)
        svc->config.storage_path = AIRY_STRDUP(config->storage_path);

    airy_mtx_init(&svc->lock);
    svc->initialized = 1;
    *service = svc;
    return 0;
}

int market_service_destroy(market_service_t *service)
{
    if (!service) {
        SVC_LOG_ERROR("market_service_destroy: NULL service parameter");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]) {
            AIRY_FREE(service->agents[i]->agent_id);
            AIRY_FREE(service->agents[i]->name);
            AIRY_FREE(service->agents[i]->version);
            AIRY_FREE(service->agents[i]->description);
            AIRY_FREE(service->agents[i]->author);
            AIRY_FREE(service->agents[i]->repository);
            AIRY_FREE(service->agents[i]->dependencies);
            AIRY_FREE(service->agents[i]);
        }
    }

    for (size_t i = 0; i < service->skill_count; i++) {
        if (service->skills[i]) {
            AIRY_FREE(service->skills[i]->skill_id);
            AIRY_FREE(service->skills[i]->name);
            AIRY_FREE(service->skills[i]->version);
            AIRY_FREE(service->skills[i]->description);
            AIRY_FREE(service->skills[i]->author);
            AIRY_FREE(service->skills[i]->repository);
            AIRY_FREE(service->skills[i]->dependencies);
            AIRY_FREE(service->skills[i]);
        }
    }

    AIRY_FREE((void *)service->config.registry_url);
    AIRY_FREE((void *)service->config.storage_path);
    airy_mtx_unlock(&service->lock);
    airy_mtx_destroy(&service->lock);
    AIRY_FREE(service);
    return 0;
}
