// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_security_signature.c
 * @brief Daemon Layer Security - package signature verification domain.
 *
 * Phase 2.3a split from daemon_security.c: ED25519 package signature
 * verification against the trusted-keys directory (market_d supply-chain
 * protection). Fail-closed: an unverifiable package is never marked valid.
 *
 * The public API surface (daemon_security.h) is unchanged by this split.
 *
 * @see agentrt/daemons/common/src/daemon_security.c (init/sanitize domain)
 * @see agentrt/daemons/common/src/daemon_security_internal.h
 */

#include "daemon_security_internal.h"

#include "airy_memory.h"
#include "error.h"

#include "airy_dirent.h"
#include "cupolas_signer_info.h"
#include "daemon_platform_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef AIRY_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

int daemon_verify_package_signature(const char *package_path, bool *is_valid,
                                    cupolas_signer_info_t *signer_info)
{
    if (!package_path || !is_valid) {
        return AIRY_ERR_INVALID_PARAM;
    }
    if (signer_info)
        AIRY_MEMSET(signer_info, 0, sizeof(cupolas_signer_info_t));

    *is_valid = false;

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR(
            "daemon_verify_package_signature: daemon_security not initialized — "
            "call daemon_cupolas_init() during startup. Marking package UNVERIFIED (fail-closed).");
        *is_valid = false;
        return AIRY_ERR_STATE_ERROR;
    }

    if (!g_security_ctx.signature_enabled) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("Signature verification: disabled by configuration, package NOT verified");
        *is_valid = false;
        return AIRY_OK;
    }
    airy_mtx_unlock(&g_security_mutex);

    struct stat st;
    if (stat(package_path, &st) != 0) {
        SVC_LOG_ERROR("Package not found: %s", package_path);
        return AIRY_ERR_NOT_FOUND;
    }

    if (st.st_size == 0) {
        SVC_LOG_ERROR("Package is empty: %s", package_path);
        return AIRY_ERR_INVALID_PARAM;
    }

    if (st.st_size > 512 * 1024 * 1024) {
        SVC_LOG_ERROR("Package exceeds size limit: %s (%lld bytes)", package_path,
                      (long long)st.st_size);
        *is_valid = false;
        return AIRY_OK;
    }

    char sig_path[1024];
    snprintf(sig_path, sizeof(sig_path), "%s.sig", package_path);
    struct stat sig_st;
    if (stat(sig_path, &sig_st) != 0) {
        SVC_LOG_WARN("No signature file found for %s (expected %s), marking as unverified",
                     package_path, sig_path);
        *is_valid = false;
        return AIRY_OK;
    }

    FILE *sig_fp = fopen(sig_path, "rb");
    if (!sig_fp) {
        SVC_LOG_ERROR("Cannot open signature file: %s", sig_path);
        *is_valid = false;
        return AIRY_OK;
    }

    uint8_t signature[256] = {0};
    size_t sig_len = fread(signature, 1, sizeof(signature), sig_fp);
    fclose(sig_fp);

    if (sig_len < 64 || sig_len > 256) {
        SVC_LOG_ERROR("Invalid signature length: %zu bytes (expected 64 for ED25519)", sig_len);
        *is_valid = false;
        return AIRY_OK;
    }

#ifdef AIRY_HAS_OPENSSL
    const char *trusted_keys_dir = getenv("AIRY_TRUSTED_KEYS_DIR");
    if (!trusted_keys_dir) {
        trusted_keys_dir = AIRY_CONFIG_DIR "/trusted_keys";
    }

    DIR *dir = opendir(trusted_keys_dir);
    if (!dir) {
        SVC_LOG_WARN("Trusted keys directory not found: %s, cannot verify signature",
                     trusted_keys_dir);
        *is_valid = false;
        return AIRY_OK;
    }

    FILE *pkg_fp = fopen(package_path, "rb");
    if (!pkg_fp) {
        closedir(dir);
        SVC_LOG_ERROR("Cannot open package file: %s", package_path);
        *is_valid = false;
        return AIRY_OK;
    }

    uint8_t *pkg_data = (uint8_t *)AIRY_MALLOC((size_t)st.st_size);
    if (!pkg_data) {
        fclose(pkg_fp);
        closedir(dir);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate package data buffer");
    }
    size_t pkg_read = fread(pkg_data, 1, (size_t)st.st_size, pkg_fp);
    fclose(pkg_fp);

    if (pkg_read != (size_t)st.st_size) {
        AIRY_FREE(pkg_data);
        closedir(dir);
        SVC_LOG_ERROR("Failed to read entire package: %s", package_path);
        *is_valid = false;
        return AIRY_OK;
    }

    bool verified = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        size_t name_len = strlen(entry->d_name);
        if (name_len < 5 || strcmp(entry->d_name + name_len - 4, ".pem") != 0)
            continue;

        char key_path[1024];
        snprintf(key_path, sizeof(key_path), "%s/%s", trusted_keys_dir, entry->d_name);

        FILE *key_fp = fopen(key_path, "r");
        if (!key_fp)
            continue;

        EVP_PKEY *pkey = PEM_read_PUBKEY(key_fp, NULL, NULL, NULL);
        fclose(key_fp);

        if (!pkey)
            continue;

        if (EVP_PKEY_base_id(pkey) != EVP_PKEY_ED25519) {
            EVP_PKEY_free(pkey);
            continue;
        }

        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        if (!md_ctx) {
            EVP_PKEY_free(pkey);
            continue;
        }

        if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
            EVP_MD_CTX_free(md_ctx);
            EVP_PKEY_free(pkey);
            continue;
        }

        int verify_result = EVP_DigestVerify(md_ctx, signature, sig_len, pkg_data, pkg_read);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);

        if (verify_result == 1) {
            verified = true;
            if (signer_info) {
                char *dot = strchr(entry->d_name, '.');
                size_t id_len = dot ? (size_t)(dot - entry->d_name) : name_len;
                if (id_len >= sizeof(signer_info->key_id))
                    id_len = sizeof(signer_info->key_id) - 1;
                __builtin_memcpy(signer_info->key_id, entry->d_name, id_len);
                signer_info->key_id[id_len] = '\0';
                signer_info->algorithm = AIRY_STRDUP("ED25519");
            }
            SVC_LOG_INFO("Package signature VERIFIED (ED25519): %s with key %s", package_path,
                         entry->d_name);
            break;
        }
    }

    closedir(dir);
    AIRY_FREE(pkg_data);

    *is_valid = verified;
    if (!verified) {
        SVC_LOG_SECURITY("Package signature INVALID: %s (no trusted key matched)", package_path);
    }
    return AIRY_OK;

#else
    SVC_LOG_WARN("OpenSSL not available: cannot perform ED25519 verification for %s, "
                 "marking as unverified (size=%lld bytes)",
                 package_path, (long long)st.st_size);
    *is_valid = false;
    return AIRY_ENOTSUP;
#endif
}
