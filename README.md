# daemons — Runtime Daemon Services (15 Daemons, M4 steady state)

> The user-space service layer of the Airymax agent runtime: fifteen independent daemon processes (M4 steady state) that together form the backend-service substrate sitting on top of the Airymax kernel.
> Leaf repository under the [agentrt](../) management repo.

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.9-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)

- **Repository:** `git@atomgit.com:openairymax/daemons.git`
- **Branch:** `develop/hubs-01`
- **Version:** 0.1.9 (aligned with agentrt management repo)

---

## Overview

**daemons** is the **user-space service layer** of the Airymax agent runtime. It is composed of **15 independent daemon processes** (M4 steady state, 0.1.9) — `gateway_d / llm_d / tool_d / sched_d / market_d / monit_d / channel_d / notify_d / hook_d / mem_d / agent_d / a2a_d / think_d / cupolas_d / maths_d` — together with a shared static library `svc_common` (in `common/`). Every daemon follows the **single-responsibility principle**: it runs as its own process, communicates with peers through the unified IPC service bus, and together they form a high-availability, scalable, pluggable micro-service architecture sitting on top of the Airymax kernel.

```
External client → gateway_d → (other daemons via ipc_service_bus) → atoms/syscall → kernel services
   (HTTP/WS/Stdio)  (daemon)
```

Design goals:

- **Service-oriented architecture** — every daemon runs independently and cooperates via IPC; each can be deployed and scaled separately.
- **Single responsibility** — each daemon owns exactly one core domain, minimizing coupling.
- **Pluggability** — daemons can be deployed, upgraded, and replaced independently without affecting other services.
- **High availability** — primary/backup switchover, circuit-breaker protection, failover, and automatic recovery.
- **Endogenous security** — `svc_common` PUBLIC-links `cupolas` (`daemon_cupolas_bootstrap.c`), so every daemon automatically inherits Cupolas security: request authentication, input sanitization, audit, sandbox.
- **Unified protocols** — all daemons communicate over JSON-RPC 2.0; MCP / A2A / OpenAI-API protocol conversion happens at the gateway boundary.

`daemons` is one of the 7 leaf repositories aggregated by the [agentrt](../) management repo, forming the **Service Layer** in the cyclic architecture (above the Gateway Layer `gateway`, below the Ecosystem Layer `sdk`/`ecosystem`). It is the topmost agentrt-internal leaf repository — every daemon dispatches business logic downward through `atoms/syscall` into the kernel.

## Module Classification

**Class — (Service / Composition layer).**

daemons is a service/composition module: it does not provide foundational primitives but composes them into running processes. It depends on `atoms` (CoreLoopThree / Syscall / TaskFlow / Memory primitives — `hook_d` directly links `airy_coreloopthree`; every daemon dispatches through `atoms/syscall`), `commons` (logging, config_unified, network, token, cost, observability, cognition, strategy — transitively through `svc_common`), `cupolas` (security dome, PUBLIC-linked by `svc_common`), `protocols` (JSON-RPC 2.0 / AgentsIPC envelope used by the IPC service bus; A2A / MCP adapters at the gateway boundary), `heapstore` (persistence for daemon state), and `gateway` (the `gateway_d` daemon wraps the gateway library). Its primary consumers are the SDK / Agent applications (over the gateway's JSON-RPC 2.0 surface) and OpenLab modules.

## Directory Structure

```
daemons/
├── CMakeLists.txt                 # Top-level build file; manages all 15 daemons + svc_common
├── Dockerfile.ci                  # CI environment Docker image
├── README.md                      # This file (English)
├── README_zh.md                   # Chinese version
├── LICENSE                        # Dual license texts (AGPL-3.0 + Apache-2.0)
├── NOTICE                         # Copyright notice
├── common/                        # Shared service library (svc_common)
│   ├── CMakeLists.txt             # svc_common static library target
│   ├── README.md                  # svc_common documentation
│   ├── include/                   # Shared headers
│   ├── src/                       # Source files (utility components)
│   └── tests/                     # svc_common unit tests
├── gateway_d/                     # API gateway daemon
├── llm_d/                         # LLM service daemon
├── tool_d/                        # Tool execution daemon (absorbs the plugin execution domain per M4)
├── sched_d/                       # Task scheduler daemon
├── market_d/                      # Application marketplace daemon
├── monit_d/                       # Observability daemon (absorbs info/observe per M4)
├── channel_d/                     # Communication channel daemon
├── notify_d/                      # Notification push daemon
├── hook_d/                        # Hook daemon (thin shell; core in atoms/coreloopthree)
├── mem_d/                         # Memory daemon (mem.* namespace, JSONL persistence)
├── agent_d/                       # Agent execution daemon (agent.* namespace)
├── a2a_d/                         # Agent-to-Agent (A2A) protocol daemon (a2a.* namespace)
├── think_d/                       # Dual-think / GRAD cognition daemon (think.* namespace)
├── cupolas_d/                     # Cupolas security-dome daemon (cupolas.* namespace)
├── maths_d/                       # Mathematics coprocessor daemon (maths.* namespace)
└── scripts/                       # Build / CI / analysis scripts
    ├── ci.sh                      # CI pipeline build script
    ├── local-ci.sh                # Local CI simulation
    ├── static-analysis.sh         # Static code analysis
    └── verify-coverage.sh         # Coverage verification
```

### svc_common Shared Library (`common/`)

The `common/` subdirectory compiles into the `svc_common` static library, which is PRIVATE-linked by every daemon. It aggregates 30+ utility components under a single ABI surface:

| Category | Components |
|----------|-----------|
| **Service framework** | `svc_common.c`, `svc_auth.c`, `svc_cache.h`, `svc_config.h`, `svc_logger.h`, `service_discovery.c`, `service_discovery_helper.c`, `daemon_bootstrap_ipc.c`, `daemon_bootstrap_sd.c`, `daemon_cupolas_bootstrap.c`, `daemon_event_driver.c`, `daemon_task_dispatcher.c` |
| **Resilience & safety** | `daemon_security.c` (circuit breaker, api_recovery, input_validator, log_sanitizer, ipc_backpressure authoritative implementations live in `commons/`) |
| **IPC & messaging** | `ipc_service_bus.c`, `ipc_client.c`, `ipc_bus_helper.c`, `daemon_bootstrap_ipc.h`, `method_dispatcher.c`, `jsonrpc_helpers.c`, `param_validator.c` |
| **Event & concurrency** | `airy_event_loop.c`, `thread_pool.c`, `refcount.c` |
| **Metrics & alerting** | `unified_metrics.c`, `alert_manager.c` |
| **Configuration** | `config_manager.c`, `daemon_defaults.h`, `daemon_errors.h`, `daemon_platform_ext.h` |
| **Memory** | `arena.c`, `tcache.c` (daemon-local allocators) |
| **Platform** | `platform_compat.c`, `compat.h`, `platform.h` |

> **P0.17 Phase 3 / IRON-6:** The authoritative definitions of `svc_common.h` and `ipc_service_bus.h` have been migrated to `commons/utils/ipc/include/`. The daemons-side headers under `common/include/` are kept as **re-export compatibility headers** so internal sources do not need immediate `#include` path changes, eliminating the atoms→daemons compile-time reverse dependency.

## Core Components

### The 15 Daemons (M4 consolidation steady state, 0.1.9)

> **M4 (0.1.9 §5):** `observe_d` / `info_d` were absorbed into `monit_d` (observability domain); `plugin_d` was absorbed into `tool_d` (execution domain). External RPC / capability semantics are now served via `monit_d:observe` / `monit_d:info` and `tool_d:plugin`; the gateway-side forwarding surface is unchanged.

| # | Daemon | Directory | Responsibility | CMake Target |
|---|--------|-----------|----------------|--------------|
| 1 | **API Gateway** | `gateway_d/` | Sole process boundary — protocol translation (HTTP / WS / SSE / MCP / A2A / OpenAI API ↔ JSON-RPC) and routing; `agent.run_stream` translated to SSE frames only (M1-1d); zero business logic | `gateway_d` |
| 2 | **LLM Service** | `llm_d/` | LLM inference service (`llm.*`): streaming completion, token counting, cost tracking, response caching | `llm_d` |
| 3 | **Tool Execution** | `tool_d/` | Tool registration / discovery, sandboxed execution, parameter validation, result caching (`tool.*`; M4 absorbed the `plugin.*` execution domain) | `tool_d` |
| 4 | **Task Scheduler** | `sched_d/` | Scheduling domain (`sched.*`): task / DAG graph scheduling + roadmap 3-tier blueprint routing (plan/absorb/cancel/replan/stats) + 4 scheduling strategies (round-robin / weighted / priority / ML) | `sched_d` |
| 5 | **Application Marketplace** | `market_d/` | Agent / Skill / Tool / Template artifact distribution, install, versioning (`market.*`) | `market_d` |
| 6 | **Monitoring & Alerting** | `monit_d/` | Observability domain (`monit.*`): metric collection (M4 absorbed the `observe` /metrics surface) and system-info queries (absorbed `info`), health checks, alert management, agent-infinite-loop detection | `monit_d` |
| 7 | **Channel Service** | `channel_d/` | Data-plane application channel (`channel.*`): socket/shm channel management and message routing (`/airy_ch_*`) | `channel_d` |
| 8 | **Notification Service** | `notify_d/` | Event fan-out pub/sub (`notify.*`, topic semantics): broadcast over WS / SSE / Unix socket with a ring event queue | `notify_d` |
| 9 | **Hook Daemon** | `hook_d/` | Thin daemon shell (`hook.*`); the hook system core lives in `atoms/coreloopthree/src/hook/`, linked via the split `airy_coreloop_hooks` library (M3 §4.2-2, no longer the full engine) | `hook_d` |
| 10 | **Memory Daemon** | `mem_d/` | Persistent memory service (`mem.*`): write / search / get / delete / recent / evolve, TF-IDF+embedding hybrid retrieval, KB knowledge base, JSONL persistence | `mem_d` |
| 11 | **Agent Execution** | `agent_d/` | Agent local lifecycle + execution-loop engine (`agent.*`): `run` / `run_stream` / `run_cancel` (M1 session registry + tool loop) + `spawn` / `invoke` / `terminate` / `cancel` / `list` / `count` + health check | `agent_d` |
| 12 | **A2A Protocol** | `a2a_d/` | Cross-agent protocol (`a2a.*`): Agent Card registration / discovery, A2A task state machine, message delivery | `a2a_d` |
| 13 | **Dual-Think Cognition** | `think_d/` | Cognition service face (`think.*`): `process` (two-pass GCCP) / `orchestrate` (7-stage pipeline) / `lang_process`·`lang_postprocess` (M3 language front-end) / `review` / `get_stats` — sole service face of the cognition engine | `think_d` |
| 14 | **Cupolas Security Dome** | `cupolas_d/` | Security PDP (`cupolas.*` / `policy.*`): permission engine + policy versioning (policy.load / activate / rollback / status, M2) + vault / entitlements / netsec + sanitizer / audit | `cupolas_d` |
| 15 | **Mathematics Coprocessor** | `maths_d/` | Mathematics engine (`maths.*`): pure-C recursive-descent numeric evaluation + optional Python symbolic backend (sympy-mcp / MCP-Mathematics) | `maths_d` |

> **Binary naming convention:** every daemon executable keeps the `*_d` suffix (`gateway_d / llm_d / tool_d / sched_d / market_d / monit_d / channel_d / notify_d / hook_d / mem_d / agent_d / a2a_d / think_d / cupolas_d / maths_d`). Per the 2026-07-05 naming decision, the module name was unified from `daemon` → `daemons` (directory, CMake target `airy_daemons`, repo `daemons.git`), and the process binary names are deliberately preserved. After the M4 consolidation the steady state is **15 binaries** (observe / info / plugin retired, capabilities absorbed into monit_d / tool_d).

> **Phase 3 refactor:** `mem_d / agent_d / a2a_d / think_d / cupolas_d` were split out of the gateway process into independent daemons (execution-body centralization). `gateway_d` now forwards their namespaces through the syscall layer (`airy_sys_svc_call`), keeping the gateway as a pure protocol boundary.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│              External clients / Agent applications            │
├──────────────────────────────────────────────────────────────┤
│   SDK (sdk-python / sdk-go / sdk-rust / sdk-typescript ...)   │
├──────────────────────────────────────────────────────────────┤
│   ★ daemons (Service Layer — 15 daemons + svc_common) ★     │
│                                                               │
│   gateway_d ─→ HTTP / WS / Stdio / MCP / A2A / OpenAI API     │
│              ↓                                                │
│   ┌────────┬────────┬────────┬────────┬────────┬─────────┐    │
│   │ llm_d  │tool_d  │sched_d │market_d│monit_d │channel_d│   │
│   ├────────┼────────┼────────┼────────┼────────┼─────────┤    │
│   │ notify_d│hook_d │mem_d   │agent_d │a2a_d   │think_d  │   │
│   ├────────┼────────┼────────┼────────┼────────┼─────────┤    │
│   │ cupolas_d│maths_d│        │        │        │         │    │
│   └────────┴────────┴────────┴────────┴────────┴─────────┘    │
│              ↑ ipc_service_bus (JSON-RPC 2.0)                 │
│   ┌─────────────────────────────────────────────────────────┐ │
│   │ common (svc_common — components, PUBLIC-links Cupolas   │ │
│   │ → every daemon inherits security)                       │ │
│   └─────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│   gateway / protocols / heapstore / cupolas                   │
├──────────────────────────────────────────────────────────────┤
│   atoms / commons / OS                                        │
└──────────────────────────────────────────────────────────────┘
```

**Internal dependency graph (svc_common ← daemon):**

```
svc_common  ←  gateway_d  ←  external clients
          ←  llm_d        ←  gateway_d
          ←  tool_d       ←  gateway_d, llm_d
          ←  sched_d      ←  gateway_d
          ←  market_d     ←  gateway_d
          ←  monit_d      ←  all daemons (metric / alert reporting)
          ←  channel_d    ←  gateway_d
          ←  notify_d     ←  monit_d (alert fan-out)
          ←  hook_d       ←  sched_d, tool_d (hook injection)
          ←  mem_d        ←  gateway_d, CLI/TUI (memory read/write)
          ←  agent_d      ←  gateway_d (agent spawn/invoke)
          ←  a2a_d        ←  gateway_d (A2A message exchange)
          ←  think_d      ←  gateway_d, CLI (dual-think / GRAD)
          ←  cupolas_d    ←  gateway_d, all daemons (security policy)
```

**Design principles:** service-oriented (independent processes, IPC cooperation); single responsibility per daemon; pluggability (deploy / upgrade / replace independently); high availability (primary/backup, circuit breaker, failover); endogenous security (Cupolas transitively linked via svc_common); unified protocols (JSON-RPC 2.0 over IPC service bus).

## Upstream Dependencies

> `svc_common` is the integration point that links the foundational modules (`commons`, `cupolas`) and exposes them to every daemon. daemons additionally depends on `atoms`, `protocols`, `heapstore`, and `gateway`.

| Dependency | Source | Purpose |
|------------|--------|---------|
| **atoms** | `agentrt/atoms/` | CoreLoopThree (cognition / execution / memory loops), Syscall entry surface, TaskFlow orchestration, Memory primitives — `hook_d` links the split `airy_coreloop_hooks` (M3 §4.2-2); every daemon dispatches business logic through `atoms/syscall` |
| **commons** | `agentrt/commons/` | Logging, config_unified, network, token, cost, observability, cognition, strategy — linked transitively through `svc_common`. The authoritative `svc_common.h` / `ipc_service_bus.h` now live here (`commons/utils/ipc/include/`) per IRON-6 |
| **cupolas** | `agentrt/cupolas/` | `svc_common` PUBLIC-links Cupolas (`daemon_cupolas_bootstrap.c`), so every daemon automatically inherits Cupolas security — request authentication, input sanitization, audit, sandbox |
| **protocols** | `agentrt/protocols/` | JSON-RPC 2.0 / AgentsIPC envelope used by the IPC service bus; A2A / MCP adapters used at the gateway boundary |
| **heapstore** | `agentrt/heapstore/` | Persistence for daemon state — `market_d` / `tool_d` / `llm_d` have dedicated data directories; registry tracks Agent / Skill / Session; token engine budgets LLM usage |
| **gateway** | `agentrt/gateway/` | `gateway_d` wraps the gateway library and exposes it as a system service |
| cJSON / libcurl / libyaml / OpenSSL | external | JSON parsing, HTTP client, YAML config, TLS — auto-detected by umbrella CMake (BAN-12) |

## Downstream Consumers

| Consumer | What they use |
|----------|---------------|
| **SDK / Agent applications** | SDK ships daemon client libraries; Agent apps invoke the runtime through the gateway's JSON-RPC 2.0 surface and consume daemon services (LLM, tool, scheduler, marketplace, etc.) |
| OpenLab applications | OpenLab modules orchestrate daemons through the JSON-RPC 2.0 API |
| Ecosystem ToolKit / Skills | Skills and ecosystem tools reach daemon services through the SDK |

## Build

### Prerequisites

- CMake ≥ 3.16
- C11 compiler (GCC / Clang / MSVC)
- cJSON library
- GTest (optional, for unit tests)
- lcov / genhtml (optional, for coverage reports)

### Build commands

```bash
# Standard build
cmake -S . -B /tmp/daemons-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/daemons-build --parallel $(nproc)

# Enable tests
cmake -S . -B /tmp/daemons-build -DBUILD_TESTS=ON
cmake --build /tmp/daemons-build --parallel $(nproc)
ctest --test-dir /tmp/daemons-build --output-on-failure

# Enable coverage
cmake -S . -B /tmp/daemons-build -DBUILD_COVERAGE=ON
cmake --build /tmp/daemons-build --parallel $(nproc)
cmake --build /tmp/daemons-build --target coverage

# Cross-platform build
cmake -S . -B /tmp/daemons-build -DBUILD_ALL_PLATFORMS=ON
```

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | `ON` | Build unit tests |
| `BUILD_COVERAGE` | `OFF` | Enable code-coverage reporting |
| `BUILD_ALL_PLATFORMS` | `OFF` | Cross-compile for all platforms |

### Build artifacts

- 15 daemon executables: `gateway_d`, `llm_d`, `tool_d`, `sched_d`, `market_d`, `monit_d`, `channel_d`, `notify_d`, `hook_d`, `mem_d`, `agent_d`, `a2a_d`, `think_d`, `cupolas_d`, `maths_d` — output to `${CMAKE_BINARY_DIR}/bin/` (observe / info / plugin retired into monit_d / tool_d after M4)
- `svc_common` — shared static library consumed (PRIVATE-linked) by every daemon
- Public headers installed under `include/agentrt/`

### Installation

```bash
cmake --install /tmp/daemons-build --prefix /opt/airymax
```

### Launching

```bash
# Start a single daemon
/tmp/daemons-build/bin/gateway_d --config gateway_config.json

# Start all daemons via the manager
./daemon_manager --start-all

# Inspect daemon status
./daemon_manager --status
```

### CI/CD scripts

| Script | Purpose |
|--------|---------|
| `scripts/ci.sh` | CI pipeline build script |
| `scripts/local-ci.sh` | Local CI simulation |
| `scripts/static-analysis.sh` | Static code analysis |
| `scripts/verify-coverage.sh` | Coverage verification |

## API

### Service lifecycle

Service state enum (`airy_svc_state_t`, defined in `commons/utils/ipc/include/svc_common.h`):

| State | Description |
|-------|-------------|
| `AIRY_SVC_STATE_NONE` | Not initialized |
| `AIRY_SVC_STATE_CREATED` | Created |
| `AIRY_SVC_STATE_INITIALIZING` | Initializing |
| `AIRY_SVC_STATE_READY` | Ready |
| `AIRY_SVC_STATE_RUNNING` | Running |
| `AIRY_SVC_STATE_PAUSED` | Paused |
| `AIRY_SVC_STATE_STOPPING` | Stopping |
| `AIRY_SVC_STATE_STOPPED` | Stopped |
| `AIRY_SVC_STATE_ZOMBIE` | Zombie (stop timeout / partial cleanup) |
| `AIRY_SVC_STATE_ERROR` | Error state |

Lifecycle progression:

```
INIT → CONFIG_LOAD → SERVICE_REGISTER → IDLE → BUSY → SHUTDOWN
 init    load cfg      register to SvcDisc   wait    handle   graceful stop
```

### Service capability flags (`airy_svc_capability_t`)

`AIRY_SVC_CAP_NONE / ASYNC / STREAMING / CANCELABLE / PAUSEABLE / THROTTLE / BATCH / PRIORITY / TIMEOUT` — each daemon advertises its capabilities via the `airy_svc_config_t.capabilities` bitmask.

### IPC service bus

| Transport | Scenario | Latency | Protocol |
|-----------|----------|---------|----------|
| Unix Socket | Same-machine daemons | < 100 μs | JSON-RPC 2.0 |
| TCP | Cross-machine daemons | < 1 ms | JSON-RPC 2.0 |
| Shared memory | High-perf data exchange | < 10 μs | custom |

### Error codes

Daemon-extended error codes are exposed through `daemon_errors.h` (re-exported via `common/include/svc_common.h`): `DAEMON_EINIT / ESTATE / EHEALTH` and other compatibility aliases layered on top of the standard `AIRY_E*` set defined in `commons/include/airy_types.h`.

### Usage example

```c
#include "svc_common.h"
#include "ipc_service_bus.h"

int main(void) {
    /* Daemon registers with the IPC service bus, advertising capabilities. */
    airy_svc_config_t cfg = {
        .name           = "my_daemon",
        .version        = "0.1.9",
        .capabilities   = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_CANCELABLE,
        .max_concurrent = 64,
        .timeout_ms     = 5000,
        .auto_start     = true,
        .enable_metrics = true,
    };
    /* svc_auth inherits Cupolas request authentication automatically. */
    return 0;
}
```

## License

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

This module is dual-licensed under the terms of either:

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt)), or
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

The full license texts are in the [LICENSE](LICENSE) file; the copyright notice is in
[NOTICE](NOTICE). You may select either license to comply with. The AGPL-3.0-or-later
terms apply by default; the Apache-2.0 alternative is provided for downstream
integration scenarios (e.g., closed-source or proprietary distribution) that the
AGPL does not accommodate.
