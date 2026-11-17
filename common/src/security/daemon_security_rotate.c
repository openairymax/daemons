/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_security_rotate.c
 * @brief daemon 安全域 - 审计日志轮转（agentrt-log-rotator 机制件）。
 *
 * A-3 根因：审计写入与分层日志是两套设施，审计侧只有裸 fwrite，没有
 * size 检查、轮转与加密，实测增长到 8 MiB 仍无上限。分层日志的
 * logging_backend_file 是模块级全局单例、无 per-file 实例 API，且契约
 * 参数（10MB×5、明文）与审计（500MB/天、365 份、zstd+加密）完全不同，
 * 因此按 §17.1 A-3 优选方案 (b) 实现独立轮转件，不复用前者。
 *
 * 机制/策略分离：本文件只提供轮转机制与契约默认值；上限、保留份数、
 * 压缩/加密开关经 aud_rot_policy() 从环境变量读入，由
 * daemon_security_init() 落进上下文，审计写入路径只问「是否超限」。
 *
 * 备份次序固定为「先压缩、后加密」，还原按相反次序执行：
 *   daemon_audit.log.<UTC>.zst.enc
 * 无 zstd 构建退化为明文备份，无 OpenSSL 构建退化为不加密，二者都只在
 * 编译期能力缺失时发生，运行期不做静默降级。
 */

#include "daemon_security_internal.h"

#include "airy_dirent.h"
#include "airy_memory.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef AIRY_HAS_ZSTD
#include <zstd.h>
#endif
#ifdef AIRY_HAS_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#define ROT_SCAN_MAX 512
#define ROT_NAME_MAX 300
#define ROT_CHUNK (64 * 1024)
#define ROT_KEY_LEN 32
#define ROT_IV_LEN 12
#define ROT_TAG_LEN 16
#define ROT_MAGIC "AIRYAUD1"
#define ROT_MAGIC_LEN 8

static const unsigned char rot_zstd_magic[4] = {0x28, 0xB5, 0x2F, 0xFD};

/* ---- 策略：契约默认值 + 排障用 env 覆盖 ---- */
void aud_rot_policy(audit_rot_policy_t *out)
{
    if (!out) {
        return;
    }
    out->max_bytes = AUD_MAX_BYTES_DEF;
    out->keep_files = AUD_KEEP_FILES_DEF;
    out->compress = 0;
    out->encrypt = 0;
#ifdef AIRY_HAS_ZSTD
    out->compress = 1;
#endif
#ifdef AIRY_HAS_OPENSSL
    out->encrypt = 1;
#endif

    const char *s = getenv("AIRY_AUDIT_MAX_BYTES");
    if (s && s[0]) {
        out->max_bytes = strtoull(s, NULL, 10);
    }
    s = getenv("AIRY_AUDIT_KEEP_FILES");
    if (s && s[0]) {
        unsigned long v = strtoul(s, NULL, 10);
        if (v > 0 && v <= ROT_SCAN_MAX) {
            out->keep_files = (unsigned int)v;
        }
    }
    s = getenv("AIRY_AUDIT_COMPRESS");
    if (s && s[0] == '0') {
        out->compress = 0;
    }
    s = getenv("AIRY_AUDIT_ENCRYPT");
    if (s && s[0] == '0') {
        out->encrypt = 0;
    }
}

/* ---- 平台小件 ---- */
static int rot_sync(FILE *fp)
{
    if (!fp) {
        return -1;
    }
    if (fflush(fp) != 0) {
        return -1;
    }
#ifdef _WIN32
    return _commit(_fileno(fp)) == 0 ? 0 : -1;
#else
    return fsync(fileno(fp)) == 0 ? 0 : -1;
#endif
}

static unsigned long long rot_size(FILE *fp)
{
    if (!fp) {
        return 0;
    }
#ifdef _WIN32
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        return 0;
    }
    __int64 n = _ftelli64(fp);
    return n > 0 ? (unsigned long long)n : 0;
#else
    if (fseeko(fp, 0, SEEK_END) != 0) {
        return 0;
    }
    off_t n = ftello(fp);
    return n > 0 ? (unsigned long long)n : 0;
#endif
}

static int rot_eq(const unsigned char *a, const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void rot_copy(unsigned char *dst, const unsigned char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void rot_split(char *dir, size_t dir_cap, char *base, size_t base_cap, const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (!slash || (bslash && bslash > slash)) {
        slash = bslash;
    }
#endif
    if (!slash) {
        snprintf(dir, dir_cap, ".");
        snprintf(base, base_cap, "%s", path);
        return;
    }
    size_t dlen = (size_t)(slash - path);
    if (dlen >= dir_cap) {
        dlen = dir_cap - 1;
    }
    for (size_t i = 0; i < dlen; i++) {
        dir[i] = path[i];
    }
    dir[dlen] = '\0';
    snprintf(base, base_cap, "%s", slash + 1);
}

/* ---- 密钥：env 指定优先，否则审计目录内自动生成（0600） ---- */
static const char *rot_key_path(void)
{
    const char *env = getenv("AIRY_AUDIT_KEY_FILE");
    if (env && env[0]) {
        return env;
    }
    return g_security_ctx.audit_key_path[0] ? g_security_ctx.audit_key_path : NULL;
}

static int rot_key_load(const char *path, unsigned char key[ROT_KEY_LEN])
{
    if (!path) {
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (fp) {
        size_t n = fread(key, 1, ROT_KEY_LEN, fp);
        fclose(fp);
        return n == ROT_KEY_LEN ? 0 : -1;
    }
#ifdef AIRY_HAS_OPENSSL
    unsigned char fresh[ROT_KEY_LEN];
    if (RAND_bytes(fresh, ROT_KEY_LEN) != 1) {
        return -1;
    }
    fp = fopen(path, "wbx");
    if (!fp) {
        /* 并发首建：让先创建者胜出，读取其密钥。 */
        fp = fopen(path, "rb");
        if (!fp) {
            return -1;
        }
        size_t n = fread(key, 1, ROT_KEY_LEN, fp);
        fclose(fp);
        return n == ROT_KEY_LEN ? 0 : -1;
    }
    size_t n = fwrite(fresh, 1, ROT_KEY_LEN, fp);
    fflush(fp);
    fclose(fp);
#ifndef _WIN32
    chmod(path, 0600);
#else
    _chmod(path, _S_IREAD | _S_IWRITE);
#endif
    if (n != ROT_KEY_LEN) {
        return -1;
    }
    rot_copy(key, fresh, ROT_KEY_LEN);
    return 0;
#else
    return -1;
#endif
}

/* ---- zstd 流式压缩 ---- */
#ifdef AIRY_HAS_ZSTD
static int rot_zstd_pack(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    void *ibuf = AIRY_MALLOC(ROT_CHUNK);
    void *obuf = AIRY_MALLOC(ROT_CHUNK);
    int rc = -1;
    if (cctx && ibuf && obuf &&
        !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3))) {
        int ok = 1;
        size_t n = 0;
        while ((n = fread(ibuf, 1, ROT_CHUNK, in)) > 0) {
            ZSTD_inBuffer inb = {ibuf, n, 0};
            while (inb.pos < inb.size) {
                ZSTD_outBuffer outb = {obuf, ROT_CHUNK, 0};
                size_t r = ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_continue);
                if (ZSTD_isError(r) || fwrite(obuf, 1, outb.pos, out) != outb.pos) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }
        if (ok && ferror(in)) {
            ok = 0;
        }
        ZSTD_inBuffer fin = {ibuf, 0, 0};
        size_t left = 1;
        while (ok && left != 0) {
            ZSTD_outBuffer outb = {obuf, ROT_CHUNK, 0};
            left = ZSTD_compressStream2(cctx, &outb, &fin, ZSTD_e_end);
            if (ZSTD_isError(left) || fwrite(obuf, 1, outb.pos, out) != outb.pos) {
                ok = 0;
            }
        }
        if (ok && rot_sync(out) == 0) {
            rc = 0;
        }
    }
    if (cctx) {
        ZSTD_freeCCtx(cctx);
    }
    AIRY_FREE(ibuf);
    AIRY_FREE(obuf);
    fclose(in);
    fclose(out);
    if (rc != 0) {
        remove(dst);
    }
    return rc;
}
#endif

/* ---- AES-256-GCM 流式加密 ---- */
#ifdef AIRY_HAS_OPENSSL
static int rot_aes_pack(const char *src, const char *dst)
{
    const char *kp = rot_key_path();
    unsigned char key[ROT_KEY_LEN];
    if (rot_key_load(kp, key) != 0) {
        return -1;
    }
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    unsigned char iv[ROT_IV_LEN];
    unsigned char tag[ROT_TAG_LEN];
    unsigned char *ibuf = AIRY_MALLOC(ROT_CHUNK);
    unsigned char *obuf = AIRY_MALLOC(ROT_CHUNK + ROT_TAG_LEN);
    EVP_CIPHER_CTX *ectx = EVP_CIPHER_CTX_new();
    int rc = -1;
    int ok = (RAND_bytes(iv, ROT_IV_LEN) == 1) && ibuf && obuf && ectx &&
             EVP_EncryptInit_ex(ectx, EVP_aes_256_gcm(), NULL, key, iv) == 1;
    if (ok && fwrite(ROT_MAGIC, 1, ROT_MAGIC_LEN, out) != ROT_MAGIC_LEN) {
        ok = 0;
    }
    if (ok && fwrite(iv, 1, ROT_IV_LEN, out) != ROT_IV_LEN) {
        ok = 0;
    }
    size_t n = 0;
    while (ok && (n = fread(ibuf, 1, ROT_CHUNK, in)) > 0) {
        int outl = 0;
        if (EVP_EncryptUpdate(ectx, obuf, &outl, ibuf, (int)n) != 1 ||
            fwrite(obuf, 1, (size_t)outl, out) != (size_t)outl) {
            ok = 0;
        }
    }
    if (ok && ferror(in)) {
        ok = 0;
    }
    if (ok) {
        int outl = 0;
        if (EVP_EncryptFinal_ex(ectx, obuf, &outl) != 1 ||
            fwrite(obuf, 1, (size_t)outl, out) != (size_t)outl) {
            ok = 0;
        }
    }
    if (ok && EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_GET_TAG, ROT_TAG_LEN, tag) == 1 &&
        fwrite(tag, 1, ROT_TAG_LEN, out) == ROT_TAG_LEN && rot_sync(out) == 0) {
        rc = 0;
    }
    if (ectx) {
        EVP_CIPHER_CTX_free(ectx);
    }
    AIRY_FREE(ibuf);
    AIRY_FREE(obuf);
    fclose(in);
    fclose(out);
    if (rc != 0) {
        remove(dst);
    }
    return rc;
}
#endif

/* ---- 备份命名：<log>.<UTC>[-n][.zst][.enc] ---- */
static int rot_backup(char *out, size_t cap, const char *path, int zst, int enc)
{
    time_t now = time(NULL);
    struct tm tm_utc;
#ifdef _WIN32
    if (gmtime_s(&tm_utc, &now) != 0) {
        return -1;
    }
#else
    if (!gmtime_r(&now, &tm_utc)) {
        return -1;
    }
#endif
    for (int seq = 0; seq < 100; seq++) {
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm_utc);
        if (seq == 0) {
            snprintf(out, cap, "%s.%s%s%s", path, stamp, zst ? ".zst" : "", enc ? ".enc" : "");
        } else {
            snprintf(out, cap, "%s.%s-%d%s%s", path, stamp, seq, zst ? ".zst" : "",
                     enc ? ".enc" : "");
        }
        FILE *probe = fopen(out, "rb");
        if (!probe) {
            return 0;
        }
        fclose(probe);
    }
    return -1;
}

/* ---- 保留份数裁剪：按文件名字典序（时间戳可排序）删最旧 ---- */
static int rot_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void rot_prune(const char *dir, const char *base, unsigned int keep)
{
    char prefix[ROT_NAME_MAX];
    snprintf(prefix, sizeof(prefix), "%s.", base);
    size_t plen = strlen(prefix);

    char *slots = AIRY_MALLOC((size_t)ROT_SCAN_MAX * ROT_NAME_MAX);
    if (!slots) {
        return;
    }
    size_t count = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent = NULL;
        while ((ent = readdir(d)) != NULL && count < ROT_SCAN_MAX) {
            if (strncmp(ent->d_name, prefix, plen) != 0) {
                continue;
            }
            snprintf(slots + count * ROT_NAME_MAX, ROT_NAME_MAX, "%s", ent->d_name);
            count++;
        }
        closedir(d);
    }
    if (count > keep) {
        qsort(slots, count, ROT_NAME_MAX, rot_cmp);
        for (size_t i = 0; i + keep < count; i++) {
            char victim[ROT_NAME_MAX + 300];
            snprintf(victim, sizeof(victim), "%s/%s", dir, slots + i * ROT_NAME_MAX);
            remove(victim);
        }
    }
    AIRY_FREE(slots);
}

/* ---- 轮转主流程（持锁调用） ---- */
static int rot_file(void)
{
    audit_rot_policy_t *p = &g_security_ctx.audit_rot;
    const char *log = g_security_ctx.audit_log_path;
    char dir[300];
    char base[ROT_NAME_MAX];
    char final[600];
    rot_split(dir, sizeof(dir), base, sizeof(base), log);

    if (g_security_ctx.audit_fp) {
        rot_sync(g_security_ctx.audit_fp);
        fclose(g_security_ctx.audit_fp);
        g_security_ctx.audit_fp = NULL;
    }

    int zst = p->compress ? 1 : 0;
    int enc = p->encrypt ? 1 : 0;
    int rc = -1;
    if (rot_backup(final, sizeof(final), log, zst, enc) == 0) {
        if (zst) {
            char packed[600];
            snprintf(packed, sizeof(packed), "%s.part", final);
#ifdef AIRY_HAS_ZSTD
            if (rot_zstd_pack(log, packed) == 0) {
                if (enc) {
#ifdef AIRY_HAS_OPENSSL
                    rc = rot_aes_pack(packed, final);
#endif
                } else {
                    remove(final);
                    rc = rename(packed, final) == 0 ? 0 : -1;
                }
            }
#endif
            remove(packed);
            if (rc == 0) {
                remove(log);
            }
        } else if (enc) {
#ifdef AIRY_HAS_OPENSSL
            rc = rot_aes_pack(log, final);
#endif
            if (rc == 0) {
                remove(log);
            }
        } else {
            rc = rename(log, final) == 0 ? 0 : -1;
        }
    }

    if (rc != 0) {
        SVC_LOG_WARN("audit rotate FAILED for %s — keep appending (no data loss)", log);
    } else {
        rot_prune(dir, base, p->keep_files);
        SVC_LOG_INFO("audit rotated: %s -> %s", log, final);
    }

    g_security_ctx.audit_fp = fopen(log, "a");
    g_security_ctx.audit_bytes = 0;
    if (!g_security_ctx.audit_fp) {
        SVC_LOG_WARN("audit reopen failed after rotation: %s", log);
        return -1;
    }
    return rc;
}

int aud_rot_init(void)
{
    aud_rot_policy(&g_security_ctx.audit_rot);
    g_security_ctx.audit_key_path[0] = '\0';

    const char *env_key = getenv("AIRY_AUDIT_KEY_FILE");
    if (g_security_ctx.audit_rot.encrypt) {
        if (env_key && env_key[0]) {
            snprintf(g_security_ctx.audit_key_path, sizeof(g_security_ctx.audit_key_path), "%s",
                     env_key);
        } else {
            char dir[300];
            char base[ROT_NAME_MAX];
            rot_split(dir, sizeof(dir), base, sizeof(base), g_security_ctx.audit_log_path);
            snprintf(g_security_ctx.audit_key_path, sizeof(g_security_ctx.audit_key_path),
                     "%s/.audit.key", dir);
        }
        unsigned char probe[ROT_KEY_LEN];
        if (rot_key_load(rot_key_path(), probe) != 0) {
            g_security_ctx.audit_rot.encrypt = 0;
            g_security_ctx.audit_key_path[0] = '\0';
            SVC_LOG_WARN("audit encryption unavailable (key load failed) — rotating without it");
        }
    }

    g_security_ctx.audit_bytes = rot_size(g_security_ctx.audit_fp);
    if (g_security_ctx.audit_rot.max_bytes == 0) {
        SVC_LOG_WARN("audit rotation DISABLED by AIRY_AUDIT_MAX_BYTES=0 (troubleshooting only)");
    } else if (g_security_ctx.audit_bytes > g_security_ctx.audit_rot.max_bytes) {
        SVC_LOG_INFO("audit file above cap at startup, rotating before first append");
    }
    return 0;
}

int aud_rot_apply(unsigned long long pending)
{
    const audit_rot_policy_t *p = &g_security_ctx.audit_rot;
    if (!g_security_ctx.audit_fp || p->max_bytes == 0) {
        return 0;
    }
    if (g_security_ctx.audit_bytes + pending <= p->max_bytes) {
        return 0;
    }
    return rot_file() == 0 ? 0 : -1;
}

/* ---- 取证还原：解密 → 解压 ---- */
static int rot_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[ROT_CHUNK];
    size_t n = 0;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok ? 0 : -1;
}

#ifdef AIRY_HAS_OPENSSL
static int rot_aes_unpack(const char *src, const char *dst)
{
    const char *kp = rot_key_path();
    unsigned char key[ROT_KEY_LEN];
    if (rot_key_load(kp, key) != 0) {
        return -1;
    }
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
#ifdef _WIN32
    _fseeki64(in, 0, SEEK_END);
    long long total = _ftelli64(in);
    _fseeki64(in, 0, SEEK_SET);
#else
    fseeko(in, 0, SEEK_END);
    off_t total = ftello(in);
    fseeko(in, 0, SEEK_SET);
#endif
    unsigned char magic[ROT_MAGIC_LEN];
    unsigned char iv[ROT_IV_LEN];
    unsigned char tag[ROT_TAG_LEN];
    int rc = -1;
    if (total < (long long)(ROT_MAGIC_LEN + ROT_IV_LEN + ROT_TAG_LEN) ||
        fread(magic, 1, ROT_MAGIC_LEN, in) != ROT_MAGIC_LEN ||
        fread(iv, 1, ROT_IV_LEN, in) != ROT_IV_LEN) {
        fclose(in);
        return -1;
    }
    long long ct = total - (ROT_MAGIC_LEN + ROT_IV_LEN + ROT_TAG_LEN);
#ifdef _WIN32
    _fseeki64(in, total - ROT_TAG_LEN, SEEK_SET);
#else
    fseeko(in, total - ROT_TAG_LEN, SEEK_SET);
#endif
    if (fread(tag, 1, ROT_TAG_LEN, in) != ROT_TAG_LEN) {
        fclose(in);
        return -1;
    }
#ifdef _WIN32
    _fseeki64(in, ROT_MAGIC_LEN + ROT_IV_LEN, SEEK_SET);
#else
    fseeko(in, ROT_MAGIC_LEN + ROT_IV_LEN, SEEK_SET);
#endif
    FILE *out = fopen(dst, "wb");
    unsigned char *ibuf = AIRY_MALLOC(ROT_CHUNK);
    unsigned char *obuf = AIRY_MALLOC(ROT_CHUNK + ROT_TAG_LEN);
    EVP_CIPHER_CTX *ectx = EVP_CIPHER_CTX_new();
    int ok = out && ibuf && obuf && ectx &&
             EVP_DecryptInit_ex(ectx, EVP_aes_256_gcm(), NULL, key, iv) == 1;
    while (ok && ct > 0) {
        size_t want = ct > ROT_CHUNK ? ROT_CHUNK : (size_t)ct;
        size_t got = fread(ibuf, 1, want, in);
        if (got != want) {
            ok = 0;
            break;
        }
        int outl = 0;
        if (EVP_DecryptUpdate(ectx, obuf, &outl, ibuf, (int)got) != 1 ||
            fwrite(obuf, 1, (size_t)outl, out) != (size_t)outl) {
            ok = 0;
        }
        ct -= (long long)got;
    }
    if (ok && EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_SET_TAG, ROT_TAG_LEN, tag) == 1) {
        int outl = 0;
        if (EVP_DecryptFinal_ex(ectx, obuf, &outl) == 1 &&
            fwrite(obuf, 1, (size_t)outl, out) == (size_t)outl) {
            rc = 0;
        }
    }
    if (ectx) {
        EVP_CIPHER_CTX_free(ectx);
    }
    AIRY_FREE(ibuf);
    AIRY_FREE(obuf);
    if (out) {
        fclose(out);
    }
    fclose(in);
    if (rc != 0) {
        remove(dst);
    }
    return rc;
}
#endif

#ifdef AIRY_HAS_ZSTD
static int rot_zstd_unpack(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    void *ibuf = AIRY_MALLOC(ROT_CHUNK);
    void *obuf = AIRY_MALLOC(ROT_CHUNK);
    int rc = -1;
    int ok = dctx && ibuf && obuf;
    size_t n = 0;
    while (ok && (n = fread(ibuf, 1, ROT_CHUNK, in)) > 0) {
        ZSTD_inBuffer inb = {ibuf, n, 0};
        while (inb.pos < inb.size) {
            ZSTD_outBuffer outb = {obuf, ROT_CHUNK, 0};
            size_t r = ZSTD_decompressStream(dctx, &outb, &inb);
            if (ZSTD_isError(r) || fwrite(obuf, 1, outb.pos, out) != outb.pos) {
                ok = 0;
                break;
            }
        }
    }
    if (ok && !ferror(in) && rot_sync(out) == 0) {
        rc = 0;
    }
    if (dctx) {
        ZSTD_freeDCtx(dctx);
    }
    AIRY_FREE(ibuf);
    AIRY_FREE(obuf);
    fclose(in);
    fclose(out);
    if (rc != 0) {
        remove(dst);
    }
    return rc;
}
#endif

int aud_rot_restore(const char *backup, const char *out_path)
{
    if (!backup || !out_path) {
        return -1;
    }
    FILE *fp = fopen(backup, "rb");
    if (!fp) {
        return -1;
    }
    unsigned char head[8] = {0};
    size_t n = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    if (n < 4) {
        return -1;
    }

    char stage[600];
    snprintf(stage, sizeof(stage), "%s.stage", out_path);
    int enc = rot_eq(head, (const unsigned char *)ROT_MAGIC, ROT_MAGIC_LEN) ? 1 : 0;
    const char *plain = backup;
    if (enc) {
#ifdef AIRY_HAS_OPENSSL
        if (rot_aes_unpack(backup, stage) != 0) {
            return -1;
        }
        plain = stage;
#else
        return -1;
#endif
    }

    fp = fopen(plain, "rb");
    if (!fp) {
        if (enc) {
            remove(stage);
        }
        return -1;
    }
    unsigned char zhead[4] = {0};
    size_t zn = fread(zhead, 1, sizeof(zhead), fp);
    fclose(fp);

    int rc;
    if (zn == 4 && rot_eq(zhead, rot_zstd_magic, 4)) {
#ifdef AIRY_HAS_ZSTD
        rc = rot_zstd_unpack(plain, out_path);
#else
        rc = -1;
#endif
    } else {
        rc = rot_copy_file(plain, out_path);
    }
    if (enc) {
        remove(stage);
    }
    return rc;
}
