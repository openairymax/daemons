/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_config.h
 * @brief Service error-code aliases shared by daemon IPC consumers.
 */

#ifndef SVC_CONFIG_H
#define SVC_CONFIG_H

#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif


#define SVC_OK AIRY_OK
#define SVC_ERR_INVALID_PARAM AIRY_ERR_INVALID_PARAM
#define SVC_ERR_IO AIRY_ERR_IO
#define SVC_ERR_OUT_OF_MEMORY AIRY_ERR_OUT_OF_MEMORY
#define SVC_ERR_PARSE_ERROR AIRY_ERR_PARSE_ERROR
#define SVC_ERR_RPC (-5001)


#ifdef __cplusplus
}
#endif

#endif /* SVC_CONFIG_H */
