// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gw_acl.c
 * @brief gateway_d ACL 域：外部协议默认授权策略。
 *
 * 策略与机制分离：ACL 引擎（daemon_security）只提供机制，本域集中登记
 * gateway 对 "external" 身份的默认策略；主装配域只负责在 PEP 初始化后
 * 触发一次登记。
 */

#include "gateway_d_internal.h"

#include "daemon_security.h"
#include "svc_logger.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Register default ACL rules for external protocols (fail-closed
 *        requires explicit authorization)
 *
 * External protocol requests use the unified identity "external" (see
 * gateway_business_handler.c):
 *   - fs_read/fs_write/fs_list basic tools allowed by default (do not block
 *     legitimate requests)
 *   - shell_run allowed by default (same as basic tools): gateway exposes
 *     shell_run as a tool schema to the LLM (GW_TOOLS_JSON); if denied by
 *     default, the LLM is ACL-blocked on first call, breaking the whole tool
 *     chain. To disable, set AIRY_GATEWAY_ACL_ALLOW_SHELL=false to deny
 *     explicitly (only "false"/"0" count as off; other values allow).
 *
 * Must run after daemon_security initialization (daemon_cupolas_init_pep).
 */
void gw_acl_register_defaults(void)
{
    daemon_security_add_acl_rule("external", "fs_read", true);
    daemon_security_add_acl_rule("external", "fs_write", true);
    daemon_security_add_acl_rule("external", "fs_list", true);

    daemon_security_add_acl_rule("external", "web_fetch", true);
    /* fs_glob/fs_grep read-only search, fs_edit controlled replacement,
     * web_search read-only web search — same trust level as the fs_* basic
     * tools, allowed by default (otherwise the LLM is ACL-blocked as soon as
     * it calls them via MCP) */
    daemon_security_add_acl_rule("external", "fs_glob", true);
    daemon_security_add_acl_rule("external", "fs_grep", true);
    daemon_security_add_acl_rule("external", "fs_edit", true);
    daemon_security_add_acl_rule("external", "web_search", true);

    /* shell_run allowed by default: same as fs_*; fixes "the LLM is
     * ACL-rejected as soon as it calls shell_run". Only explicitly setting
     * AIRY_GATEWAY_ACL_ALLOW_SHELL=false/0 denies it. */
    const char *shell = getenv("AIRY_GATEWAY_ACL_ALLOW_SHELL");
    bool shell_allowed = !(shell && (strcmp(shell, "false") == 0 || strcmp(shell, "0") == 0));
    daemon_security_add_acl_rule("external", "shell_run", shell_allowed);

    SVC_LOG_INFO("Phase 3: Gateway ACL defaults registered "
                 "(external: fs_read/fs_write/fs_list ALLOW, shell_run=%s)",
                 shell_allowed ? "ALLOW" : "DENY");
}
