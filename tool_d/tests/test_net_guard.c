// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_net_guard.c
 * @brief S-P1-07 验收：web_fetch 的 SSRF 主机策略与 URL 解析。
 *
 * 覆盖：环回/私网/链路本地/元数据/保留网段（IPv4 与 IPv6，含 IPv4 映射）、
 * 非规范数字形态（十进制/十六进制/前导零）、本地域名后缀、显式放行列表。
 * 全部用字面量，不做真实网络请求。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin/builtin_net_internal.h"
#include "test_framework.h"

/* 跨平台环境变量写入；value 为 NULL 表示删除。 */
static void set_env(const char *key, const char *value)
{
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value)
        setenv(key, value, 1);
    else
        unsetenv(key);
#endif
}

static void test_blocked_literals(void)
{
    static const char *const denied[] = {
        "127.0.0.1",     "127.255.255.254", "10.1.2.3",       "172.16.0.1",
        "172.31.255.255", "192.168.1.1",    "169.254.169.254", "0.0.0.0",
        "100.64.0.1",    "198.18.0.1",      "224.0.0.1",      "255.255.255.255",
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        char why[160];
        assert_int_equal(net_ssrf_check(denied[i], why, sizeof(why)), -1);
        assert_true(why[0] != '\0');
    }
}

static void test_blocked_ipv6(void)
{
    static const char *const denied[] = {
        "::",       "::1",         "fe80::1",          "febf::1",
        "fc00::1",  "fd12:3456::1", "ff02::1",         "::ffff:127.0.0.1",
        "::ffff:169.254.169.254",  "[::1]",
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        char why[160];
        assert_int_equal(net_ssrf_check(denied[i], why, sizeof(why)), -1);
    }
    char why[160];
    assert_int_equal(net_ssrf_check("2606:4700:4700::1111", why, sizeof(why)), 0);
}

static void test_blocked_names(void)
{
    static const char *const denied[] = {
        "localhost", "foo.localhost", "printer.local", "metadata.google.internal",
        "router.lan", "nas.home.arpa",
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        char why[160];
        assert_int_equal(net_ssrf_check(denied[i], why, sizeof(why)), -1);
    }

    /* 非规范数字形态可能被 curl/inet_aton 解释为内网地址，必须拒绝。 */
    static const char *const numeric[] = {"2130706433", "0x7f000001", "0177.0.0.1",
                                          "127.1"};
    for (size_t i = 0; i < sizeof(numeric) / sizeof(numeric[0]); i++) {
        char why[160];
        assert_int_equal(net_ssrf_check(numeric[i], why, sizeof(why)), -1);
    }
}

static void test_public_literals(void)
{
    static const char *const allowed[] = {"8.8.8.8", "1.1.1.1", "93.184.216.34"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        char why[160];
        assert_int_equal(net_ssrf_check(allowed[i], why, sizeof(why)), 0);
        assert_string_equal(why, "");
    }
}

static void test_allow_list(void)
{
    char why[160];

    set_env("AIRY_NET_ALLOW_HOSTS", "169.254.169.254, Printer.LAN");
    assert_int_equal(net_ssrf_check("169.254.169.254", why, sizeof(why)), 0);
    assert_int_equal(net_ssrf_check("printer.lan", why, sizeof(why)), 0);
    assert_int_equal(net_ssrf_check("127.0.0.1", why, sizeof(why)), -1);

    set_env("AIRY_NET_ALLOW_HOSTS", "*");
    assert_int_equal(net_ssrf_check("127.0.0.1", why, sizeof(why)), 0);
    set_env("AIRY_NET_ALLOW_HOSTS", NULL);
}

static void test_url_parse(void)
{
    builtin_url_t u;

    assert_int_equal(net_url_parse("https://example.com/a?b=1", &u), 0);
    assert_string_equal(u.scheme, "https");
    assert_string_equal(u.host, "example.com");
    assert_int_equal(u.port, 443);
    assert_string_equal(u.path, "/a?b=1");

    assert_int_equal(net_url_parse("http://10.0.0.1:8080", &u), 0);
    assert_int_equal(u.port, 8080);
    assert_string_equal(u.path, "/");

    assert_int_equal(net_url_parse("ftp://example.com/", &u), -1);
    assert_int_equal(net_url_parse("example.com", &u), -1);
    assert_int_equal(net_url_parse("http://example.com:0/", &u), -1);
}

int main(void)
{
    set_env("AIRY_NET_ALLOW_HOSTS", NULL);
    test_blocked_literals();
    test_blocked_ipv6();
    test_blocked_names();
    test_public_literals();
    test_allow_list();
    test_url_parse();
    printf("test_net_guard: all checks passed\n");
    return 0;
}
