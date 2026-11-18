// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file network_pool.c
 * @brief Network module - connection pool domain.
 *
 * Implements the connection pool create/destroy/get/release/health-check
 * functions declared in network_common.h.
 * Following ARCHITECTURAL_PRINCIPLES.md design principles:
 * - E-4 Cross-platform consistency: Windows/Linux/macOS
 * - E-5 Semantic naming: every function name states its purpose
 * - E-6 Traceable errors: unified error code system
 */

#include "network_common_internal.h"

network_pool_t *network_pool_create(const network_config_t *config, size_t pool_size)
{
    if (!config || pool_size == 0 || pool_size > NETWORK_MAX_POOL_SIZE) {
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    network_pool_t *pool = (network_pool_t *)AIRY_CALLOC(1, sizeof(network_pool_t));
    if (!pool) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    pool->base_config = *config;
    pool->max_size = pool_size;
    pool->current_size = 0;
    pool->connections =
        (network_connection_t **)AIRY_CALLOC(pool_size, sizeof(network_connection_t *));

    if (!pool->connections) {
        AIRY_FREE(pool);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    return pool;
}

void network_pool_destroy(network_pool_t *pool)
{
    if (!pool) {
        return;
    }

    for (size_t i = 0; i < pool->current_size; i++) {
        if (pool->connections[i]) {
            network_connection_destroy(pool->connections[i]);
        }
    }

    AIRY_FREE(pool->connections);
    AIRY_FREE(pool);
}

network_connection_t *network_pool_acquire(network_pool_t *pool, int timeout_ms)
{
    if (!pool) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    (void)timeout_ms;

    for (size_t i = 0; i < pool->current_size; i++) {
        if (pool->connections[i] &&
            network_get_status(pool->connections[i]) == NETWORK_STATUS_CONNECTED) {
            return pool->connections[i];
        }
    }

    if (pool->current_size < pool->max_size) {
        network_connection_t *conn = network_connection_create(&pool->base_config);
        if (!conn) {
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }

        airy_err_t err = network_connect(conn);
        if (err != AIRY_SUCCESS) {
            network_connection_destroy(conn);
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }

        pool->connections[pool->current_size] = conn;
        pool->current_size++;

        return conn;
    }

    return NULL;
}

void network_pool_release(network_pool_t *pool, network_connection_t *connection)
{
    if (!pool || !connection) {
        return;
    }
}

size_t network_pool_available(const network_pool_t *pool)
{
    if (!pool) {
        return 0;
    }

    size_t available = 0;
    for (size_t i = 0; i < pool->current_size; i++) {
        if (pool->connections[i] &&
            network_get_status(pool->connections[i]) == NETWORK_STATUS_CONNECTED) {
            available++;
        }
    }

    if (pool->current_size < pool->max_size) {
        available += (pool->max_size - pool->current_size);
    }

    return available;
}

size_t network_pool_size(const network_pool_t *pool)
{
    if (!pool) {
        return 0;
    }
    return pool->current_size;
}

size_t network_pool_health_check(network_pool_t *pool)
{
    if (!pool) {
        return 0;
    }

    size_t healthy = 0;

    for (size_t i = 0; i < pool->current_size;) {
        if (pool->connections[i]) {
            network_status_t status = network_get_status(pool->connections[i]);

            if (status == NETWORK_STATUS_CONNECTED) {
                healthy++;
                i++;
            } else if (status == NETWORK_STATUS_ERROR || status == NETWORK_STATUS_DISCONNECTED) {

                network_connection_destroy(pool->connections[i]);

                if (i < pool->current_size - 1) {
                    pool->connections[i] = pool->connections[pool->current_size - 1];
                    pool->connections[pool->current_size - 1] = NULL;
                } else {
                    pool->connections[i] = NULL;
                }
                pool->current_size--;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }

    return healthy;
}
