/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file method_dispatcher.h
 * @brief JSON-RPC method dispatcher framework (registry-based routing).
 *
 * Design goals:
 * 1. Eliminate the if-else method routing chain, lowering cyclomatic complexity
 * 2. Unify request handling across all JSON-RPC services
 * 3. Support dynamic method registration and runtime extension
 * 4. Thread-safe dispatch mechanism
 *
 * Usage example:
 * @code
 *   // Create a dispatcher
 *   method_dispatcher_t* disp = method_dispatcher_create(16);
 *
 *   // Register a method handler
 *   method_dispatcher_register(disp, "my_method", on_my_method, NULL);
 *
 *   // Dispatch a request (auto-invokes the matching handler)
 *   method_dispatcher_dispatch(disp, request, jsonrpc_build_error, &client_fd);
 *
 *   // Clean up
 *   method_dispatcher_destroy(disp);
 * @endcode
 *
 * Performance:
 * - Register: O(1) hash-table insert
 * - Dispatch: O(1) average lookup
 * - Memory: linear in the number of methods
 */

#ifndef AIRY_RT_METHOD_DISPATCHER_H
#define AIRY_RT_METHOD_DISPATCHER_H

#include <cjson/cJSON.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct method_dispatcher method_dispatcher_t;

/**
 * @brief Method-handler function type signature.
 * @param params params object of the JSON-RPC request
 * @param id Request ID (for building the response)
 * @param user_data User context data (passed at registration)
 * @note All method handlers must match this signature
 */
typedef void (*method_fn)(cJSON *params, int id, void *user_data);

/**
 * @brief Create a method-dispatcher instance.
 * @param max_methods Max number of supported methods
 * @return Dispatcher pointer, NULL on failure
 *
 * Internally allocates a hash table plus array storage, O(1) space
 */
method_dispatcher_t *method_dispatcher_create(size_t max_methods);

/**
 * @brief Destroy the dispatcher and release all resources.
 * @param disp Dispatcher instance (may be NULL, safe no-op)
 *
 * Note: does not destroy the user_data passed at registration
 */
void method_dispatcher_destroy(method_dispatcher_t *disp);

/**
 * @brief Register a method handler.
 * @param disp Dispatcher instance
 * @param method Method name (e.g. "complete", "register")
 * @param handler Handler function pointer
 * @param user_data User data passed to the handler (may be NULL)
 * @return 0 on success, -1 on failure (already exists or invalid params)
 *
 * If the method already exists, the old handler is overwritten with a
 * warning log
 */
int method_dispatcher_register(method_dispatcher_t *disp, const char *method, method_fn handler,
                               void *user_data);

/**
 * @brief Dispatch a JSON-RPC request to the matching handler.
 * @param disp Dispatcher instance
 * @param request Full JSON-RPC request object
 * @param error_response_fn Error-response builder (used when method not found)
 * @param user_data User data passed to the handler (usually client_fd)
 * @return 0 on successful dispatch, -1 if method not found or on error
 *
 * Flow:
 * 1. Extract the "method" field from request
 * 2. Look up the matching handler in the registry
 * 3. Call the handler with params, id, user_data
 * 4. If not found, call error_response_fn to build the error response
 */
int method_dispatcher_dispatch(method_dispatcher_t *disp, cJSON *request,
                               char *(*error_response_fn)(int, const char *, int), void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_METHOD_DISPATCHER_H */
