/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_net_http.c
 * @brief Redirect-safe HTTP(S) GET shared by web_fetch and web_search.
 *
 *        Requests are issued in process through the providers transport
 *        (airy_prov/provider_transport.h) instead of an external curl child
 *        process: slim Windows/macOS images ship no curl binary, and a
 *        missing binary made every fetch indistinguishable from being
 *        offline. Transport failures now carry the failing stage
 *        (DNS/TCP/TLS/timeout) into resp->why.
 *
 *        Redirects are never followed inside libcurl: every hop is a single
 *        non-following GET, and the full URL policy (net_url_guard, SSRF
 *        host rules) is re-run on each Location before the next hop is
 *        issued. A model-supplied URL therefore cannot pass the first hop
 *        and then be redirected into the cloud metadata service or loopback.
 */

#include "builtin/builtin_net_internal.h"

#include "airy_prov/provider_transport.h"

#include "builtin/builtin_limits_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NET_HTTP_MAX_HOPS 5U
#define NET_HTTP_MIN_MS 500U
#define NET_HTTP_DEFAULT_HOP_MS 20000U
#define NET_HTTP_URL_CAP 4096U

/* 本跳预算：整链预算已耗尽返回 0，无预算返回默认值。 */
static uint32_t hop_budget(uint64_t deadline)
{
    int left = builtin_dl_left_ms(deadline);

    if (left == 0)
        return 0;
    if (left < 0)
        return NET_HTTP_DEFAULT_HOP_MS;
    return (uint32_t)left;
}

/* 响应体上限由传输层在累积时执行，与旧实现的 1MB 读上限一致。 */
int net_http_get(const net_http_req_t *req, net_http_resp_t *resp)
{
    char cur[NET_HTTP_URL_CAP];

    if (!req || !resp || !req->url || !req->url[0])
        return -1;
    AIRY_MEMSET(resp, 0, sizeof(*resp));
    if (strlen(req->url) >= sizeof(cur)) {
        snprintf(resp->why, sizeof(resp->why), "request URL too long");
        return -1;
    }
    AIRY_MEMCPY(cur, req->url, strlen(req->url) + 1);

    uint64_t deadline = builtin_deadline_ms(req->timeout_ms);
    int hops = 0;

    for (;;) {
        uint32_t budget_ms = hop_budget(deadline);
        if (budget_ms < NET_HTTP_MIN_MS) {
            snprintf(resp->why, sizeof(resp->why), "time budget exhausted before the request");
            return -1;
        }

        provider_hop_req_t hreq;
        hreq.url = cur;
        hreq.headers = req->headers;
        hreq.timeout_sec = (double)budget_ms / 1000.0;
        hreq.max_body = BUILTIN_OUTPUT_CAP;

        provider_hop_resp_t hout;
        int rc = provider_http_hop_get(&hreq, &hout);
        if (rc != AIRY_OK) {
            snprintf(resp->why, sizeof(resp->why), "http transport: %s",
                     hout.why[0] ? hout.why : "failed");
            return -1;
        }

        long code = hout.http_code;
        if (code >= 300 && code < 400 && hout.location && hout.location[0]) {
            char why[160];
            size_t len = strlen(hout.location);
            if (hops >= (int)NET_HTTP_MAX_HOPS) {
                snprintf(resp->why, sizeof(resp->why),
                         "too many redirects (more than %u)", (unsigned)NET_HTTP_MAX_HOPS);
            } else if (len >= sizeof(cur)) {
                snprintf(resp->why, sizeof(resp->why), "redirect URL too long");
            } else if (net_url_guard(hout.location, why, sizeof(why)) != 0) {
                snprintf(resp->why, sizeof(resp->why), "redirect rejected: %s", why);
            } else {
                AIRY_MEMCPY(cur, hout.location, len + 1);
                AIRY_FREE(hout.location);
                provider_http_resp_free(hout.response);
                hops++;
                continue;
            }
            AIRY_FREE(hout.location);
            provider_http_resp_free(hout.response);
            resp->status = code;
            return -1;
        }

        AIRY_FREE(hout.location);
        resp->body = hout.response->data;
        hout.response->data = NULL;
        provider_http_resp_free(hout.response);
        if (!resp->body)
            resp->body = AIRY_STRDUP("");
        resp->status = code;
        resp->exit_code = 0;
        resp->redirects = hops;
        return 0;
    }
}
