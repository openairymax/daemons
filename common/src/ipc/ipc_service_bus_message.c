// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ipc_service_bus_message.c
 * @brief IPC service-bus implementation - message domain.
 *
 * Phase 2.3a split from ipc_service_bus.c: message factory/lifecycle
 * (create/free/clone), the shared [SC] A-IPC message-header initializer,
 * payload CRC32 checksum and protocol string conversion helpers.
 *
 * The public API surface (ipc_service_bus.h) is unchanged by this split.
 *
 * @see agentrt/daemons/common/src/ipc_service_bus.c (bus core domain)
 * @see agentrt/daemons/common/src/ipc_service_bus_internal.h
 */

#include "ipc_service_bus_internal.h"

#include "airy_memory.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "error.h"

static uint32_t compute_checksum(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

void init_message_header(ipc_bus_message_header_t *header, ipc_bus_msg_type_t msg_type,
                                ipc_bus_proto_t protocol, const char *source, const char *target)
{
    AIRY_MEMSET(header, 0, sizeof(ipc_bus_message_header_t));
    /* [SC] A-IPC 标准头（Layout C v4 前置 128B，与 agentrt-linux wire 兼容） */
    header->aipc.magic = AIRY_IPC_MAGIC;
    header->aipc.opcode = AIRY_IPC_OP_SEND;
    /* 扩展段：service bus 语义字段 */
    header->msg_type = msg_type;
    header->protocol = protocol;
    header->timestamp = airy_time_ms();
    if (source)
        safe_strcpy(header->source, source, IPC_BUS_SERVICE_ID_LEN);
    if (target)
        safe_strcpy(header->target, target, IPC_BUS_SERVICE_ID_LEN);
}

AIRY_API ipc_bus_message_t *ipc_bus_message_create(ipc_bus_msg_type_t msg_type,
                                                   ipc_bus_proto_t protocol, const void *payload,
                                                   size_t payload_size)
{
    ipc_bus_message_t *msg = (ipc_bus_message_t *)AIRY_CALLOC(1, sizeof(ipc_bus_message_t));
    if (!msg) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    init_message_header(&msg->header, msg_type, protocol, NULL, NULL);
    msg->header.msg_id = (uint64_t)airy_time_ms();
    msg->header.aipc.payload_len = (uint32_t)payload_size;

    if (payload && payload_size > 0) {
        msg->payload = AIRY_CALLOC(1, payload_size);
        if (!msg->payload) {
            AIRY_FREE(msg);
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }

        __builtin_memcpy(msg->payload, payload, payload_size);
        msg->payload_size = payload_size;
        msg->header.checksum = compute_checksum(payload, payload_size);
        msg->header.aipc.crc32 = msg->header.checksum; /* [SC] A-IPC crc32 同步 */
    }

    return msg;
}

AIRY_API void ipc_bus_message_free(ipc_bus_message_t *message)
{
    if (!message)
        return;
    if (message->payload) {
        AIRY_FREE(message->payload);
        message->payload = NULL;
    }
    AIRY_FREE(message);
}

AIRY_API ipc_bus_message_t *ipc_bus_message_clone(const ipc_bus_message_t *message)
{
    if (!message) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    ipc_bus_message_t *clone =
        ipc_bus_message_create(message->header.msg_type, message->header.protocol, message->payload,
                               message->payload_size);
    if (!clone) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    clone->header = message->header;
    return clone;
}

AIRY_API const char *ipc_bus_proto_to_string(ipc_bus_proto_t proto)
{
    static const char *proto_strings[] = {"JSON-RPC", "MCP", "A2A", "OpenAI", "AUTO"};

    if (proto < 0 || proto > IPC_BUS_PROTO_AUTO)
        return "UNKNOWN";
    return proto_strings[proto];
}

AIRY_API ipc_bus_proto_t ipc_bus_proto_from_string(const char *str)
{
    if (!str)
        return IPC_BUS_PROTO_AUTO;

    if (strcasecmp(str, "JSON-RPC") == 0 || strcasecmp(str, "jsonrpc") == 0)
        return IPC_BUS_PROTO_JSON_RPC;
    if (strcasecmp(str, "MCP") == 0)
        return IPC_BUS_PROTO_MCP;
    if (strcasecmp(str, "A2A") == 0)
        return IPC_BUS_PROTO_A2A;
    if (strcasecmp(str, "OpenAI") == 0 || strcasecmp(str, "openai") == 0)
        return IPC_BUS_PROTO_OPENAI;

    return IPC_BUS_PROTO_AUTO;
}
