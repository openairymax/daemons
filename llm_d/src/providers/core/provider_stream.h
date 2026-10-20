/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_stream.h: Provider 域流式传输与分帧（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_STREAM_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_STREAM_H

#include "llm_service_types.h"
#include "provider_retry_class.h"
#include <curl/curl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 追加 len 字节并保持 NUL 结尾，容量不足时倍增；返回（可能搬迁的）缓冲，
 * 失败返回 NULL 且原缓冲仍有效。 */
char *provider_buf_append_n(char *buf, size_t *cap, size_t *len, const char *data, size_t dlen);

/* 同 provider_buf_append_n，长度取 strlen(text)。 */
char *provider_buf_append(char *buf, size_t *cap, size_t *len, const char *text);

typedef int (*provider_stream_chunk_cb_t)(const char *data_line, void *user_data);

typedef int (*provider_sse_event_cb_t)(const char *event, const char *data, size_t data_len,
                                       void *user_data);

/* 流式 POST·行协议：SSE data 行交付 on_chunk，"[DONE]" 置正常终止。 */
int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, int max_retries,
                              provider_retry_class_t retry_class,
                              provider_stream_chunk_cb_t on_chunk, void *chunk_user_data,
                              long *out_http_code);

/* 流式 POST·具名事件：event:/data: 双行解析，事件名随载荷交付 on_event。 */
int provider_http_post_stream_sse(const char *url, struct curl_slist *headers, const char *body,
                                  double timeout_sec, int max_retries,
                                  provider_retry_class_t retry_class,
                                  provider_sse_event_cb_t on_event, void *event_user_data,
                                  long *out_http_code);

/* 控制帧发射：工具帧 RS 'T' <json> RS；推理帧 RS 'R' <reasoning> RS。 */
void provider_emit_tool_frame(llm_stream_callback_t cb, void *ud, const char *tc_json);
void provider_emit_reasoning_frame(llm_stream_callback_t cb, void *ud, const char *reasoning);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_STREAM_H */
