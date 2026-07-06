**Language:** English | [简体中文](README_zh.md)

# Airymax Daemons — Runtime Daemon Services

`agentrt/daemons/`

**Version:** 0.1.1
**License:** AGPL-3.0-or-later OR Apache-2.0 (dual-licensed)
**Branch:** `feature/official-hubs-01`

---

## 1. Module Positioning

Daemons is the **user-space service layer** of the Airymax agent runtime. It is
composed of **12 independent daemon processes** that together provide the full
backend-service substrate for the agent system. Every daemon follows the
**single-responsibility principle**: it runs as its own process, communicates
with peers through a unified IPC service bus, and together they form a
high-availability, scalable, pluggable micro-service architecture sitting on
top of the Airymax kernel.

Design goals:

- **Service-oriented architecture** — every daemon runs independently and
  cooperates via IPC; each can be deployed and scaled separately.
- **Single responsibility** — each daemon owns exactly one core domain,
  minimizing coupling.
- **Pluggability** — daemons can be deployed, upgraded, and replaced
  independently without affecting other services.
- **High availability** — primary/backup switchover, circuit-breaker
  protection, failover, and automatic recovery.
- **Endogenous security** — every daemon links `svc_common`, which
  transitively links Cupolas; every request is authenticated under a
  zero-trust model.
- **Unified protocols** — all daemons communicate over JSON-RPC 2.0, with
  MCP / A2A / OpenAI-API protocol conversion supported at the gateway.

---

## 2. Directory Structure

```
daemons/
├── CMakeLists.txt                 # Top-level build file, manages all sub-modules
├── Dockerfile.ci                  # CI environment Docker image
├── README.md                      # This file (English)
├── README_zh.md                   # Chinese version
├── LICENSE                        # Dual license texts (AGPL-3.0 + Apache-2.0)
├── NOTICE                         # Copyright notice
├── common/                        # Shared service library (svc_common, 18+ components)
├── gateway_d/                     # API gateway daemon
├── llm_d/                         # LLM service daemon
├── tool_d/                        # Tool execution daemon
├── sched_d/                       # Task scheduler daemon
├── market_d/                      # Application marketplace daemon
├── monit_d/                       # Monitoring & alerting daemon
├── channel_d/                     # Communication channel daemon
├── info_d/                        # Information service daemon
├── notify_d/                      # Notification push daemon
├── observe_d/                     # Observability (OpenTelemetry) daemon
├── hook_d/                        # Hook daemon (thin shell; core in atoms/coreloopthree)
├── plugin_d/                      # Plugin daemon
├── examples/                      # Usage examples (example_svc_usage.c)
└── scripts/                       # Build / CI / analysis scripts
    ├── ci.sh
    ├── local-ci.sh
    ├── static-analysis.sh
    └── verify-coverage.sh
```

### The 12 Daemons

| Daemon | Directory | Responsibility | CMake Target |
|--------|-----------|----------------|--------------|
| **API Gateway** | `gateway_d/` | External request intake, protocol conversion (HTTP / WS / MCP / A2A / OpenAI API) and routing | `gateway_d` |
| **LLM Service** | `llm_d/` | Large-language-model invocation, token counting, cost tracking, response caching | `llm_d` |
| **Tool Execution** | `tool_d/` | Tool registration / discovery, sandboxed execution, parameter validation, result caching | `tool_d` |
| **Task Scheduler** | `sched_d/` | Task dispatch with 4 scheduling strategies (round-robin / weighted / priority / ML) | `sched_d` |
| **Application Marketplace** | `market_d/` | Agent / Skill / Tool / Template resource management, install, versioning | `market_d` |
| **Monitoring & Alerting** | `monit_d/` | Metric collection, health checks, alert management, agent-infinite-loop detection | `monit_d` |
| **Channel Service** | `channel_d/` | Communication-channel management and message routing | `channel_d` |
| **Information Service** | `info_d/` | System information query and status reporting | `info_d` |
| **Notification Service** | `notify_d/` | Multi-channel notification push (email / Slack / Discord) | `notify_d` |
| **Observability Service** | `observe_d/` | OpenTelemetry observability data collection | `observe_d` |
| **Hook Daemon** | `hook_d/` | Thin daemon shell; the hook system core lives in `atoms/coreloopthree/src/hook/` and is obtained by linking `agentrt_coreloopthree` | `hook_d` |
| **Plugin Daemon** | `plugin_d/` | Plugin lifecycle management and isolation | `plugin_d` |
| **Shared Library** | `common/` | Shared utility library and compatibility layer (18+ components) — `svc_common` | `svc_common` |

### Architecture Overview

```
+-------------------------------------------------------------------+
|                  External clients / Agents                         |
+-------------------------------------------------------------------+
|  gateway_d (API gateway)                                           |
|  HTTP / WS / Stdio / MCP / A2A / OpenAI API → JSON-RPC 2.0 → route |
+---+---------------+---------------+---------------+---------------+
    |               |               |               |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  |  llm_d    |  |  tool_d   |  |  sched_d  |  | market_d  |      |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  |  monit_d  |  | channel_d |  |  info_d   |  | notify_d  |      |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  +-----------+  +-----------+  +--------------------------------+ |
|  | observe_d |  |  hook_d   |  |  plugin_d                      | |
|  +-----------+  +-----------+  +--------------------------------+ |
|  +--------------------------------------------------------------+ |
|  |  common (svc_common — 18+ components, transitively links Cupolas) |
|  +--------------------------------------------------------------+ |
+-------------------------------------------------------------------+
|                    Airymax kernel (atoms)                          |
+-------------------------------------------------------------------+
```

---

## 3. Upstream / Downstream Dependencies

### Upstream (Daemons depend on)

| Dependency | Source | Purpose |
|------------|--------|---------|
| **atoms** | `agentrt/atoms/` | CoreLoopThree (cognition / execution / memory loops), Syscall entry surface, TaskFlow orchestration, Memory primitives — `hook_d` directly links `agentrt_coreloopthree`; every daemon dispatches business logic through `atoms/syscall` |
| **commons** | `agentrt/commons/` | Logging, config_unified, network, token, cost, observability, cognition, strategy — linked transitively through `svc_common` |
| **cupolas** | `agentrt/cupolas/` | `svc_common` links Cupolas as `PUBLIC` (`daemon_cupolas_bootstrap.c`), so every daemon automatically inherits Cupolas security — request authentication, input sanitization, audit, sandbox |
| **protocols** | `agentrt/protocols/` | JSON-RPC 2.0 / AgentsIPC envelope used by the IPC service bus; A2A / MCP adapters used at the gateway boundary |
| **heapstore** | `agentrt/heapstore/` | Persistence for daemon state — `market_d` / `tool_d` / `llm_d` have dedicated data directories; registry tracks Agent / Skill / Session; token engine budgets LLM usage |
| **gateway** | `agentrt/gateway/` | `gateway_d` wraps the gateway library and exposes it as a system service |
| cJSON / libcurl / libyaml / OpenSSL | external | JSON parsing, HTTP client, YAML config, TLS — auto-detected by umbrella CMake |

### Downstream (consumers of Daemons)

| Consumer | What it uses |
|----------|--------------|
| **SDK / Agent applications** | SDK ships daemon client libraries; Agent apps invoke the runtime through the gateway's JSON-RPC 2.0 surface and consume daemon services (LLM, tool, scheduler, marketplace, etc.) |
| OpenLab applications | OpenLab modules orchestrate daemons through the JSON-RPC 2.0 API |

### Internal Dependency Graph

```
svc_common  ←  gateway_d  ←  external clients
          ←  llm_d        ←  gateway_d
          ←  tool_d       ←  gateway_d, llm_d
          ←  sched_d      ←  gateway_d
          ←  market_d     ←  gateway_d
          ←  monit_d      ←  all daemons (metric reporting)
          ←  channel_d    ←  gateway_d
          ←  info_d       ←  gateway_d
          ←  notify_d     ←  monit_d (alert notifications)
          ←  observe_d    ←  monit_d (observability)
          ←  hook_d       ←  sched_d, tool_d (hook injection)
          ←  plugin_d     ←  market_d, tool_d (plugin lifecycle)
```

---

## 4. Communication & Lifecycle

### IPC Service Bus

All daemons communicate through the unified `ipc_service_bus`, supporting
multi-protocol messaging:

| Transport | Scenario | Latency | Protocol |
|-----------|----------|---------|----------|
| Unix Socket | Same-machine daemons | < 100 μs | JSON-RPC 2.0 |
| TCP | Cross-machine daemons | < 1 ms | JSON-RPC 2.0 |
| Shared memory | High-perf data exchange | < 10 μs | custom |

### Daemon Lifecycle

```
INIT → CONFIG_LOAD → SERVICE_REGISTER → IDLE → BUSY → SHUTDOWN
 init    load cfg      register to SvcDisc   wait    handle   graceful stop
```

Service state enum (`agentrt_svc_state_t`): `NONE / CREATED / INITIALIZING /
READY / RUNNING / PAUSED / STOPPING / STOPPED / ZOMBIE / ERROR`.

---

## 5. Build Instructions

### Prerequisites

- CMake ≥ 3.16
- C11 compiler (GCC / Clang / MSVC)
- cJSON library
- GTest (optional, for unit tests)
- lcov / genhtml (optional, for coverage reports)

### Build Commands

```bash
# Standard build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Enable tests
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build

# Enable coverage
cmake -B build -DBUILD_COVERAGE=ON
cmake --build build
cmake --build build --target coverage

# Cross-platform build
cmake -B build -DBUILD_ALL_PLATFORMS=ON
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | `ON` | Build unit tests |
| `BUILD_COVERAGE` | `OFF` | Enable code-coverage reporting |
| `BUILD_ALL_PLATFORMS` | `OFF` | Cross-compile for all platforms |

### Build Artifacts

- 12 daemon executables: `gateway_d`, `llm_d`, `tool_d`, `sched_d`,
  `market_d`, `monit_d`, `channel_d`, `info_d`, `notify_d`, `observe_d`,
  `hook_d`, `plugin_d` — output to `${CMAKE_BINARY_DIR}/bin/`
- `svc_common` — shared static library consumed by every daemon
- Public headers installed under `include/agentrt/`

### Installation

```bash
cmake --install build --prefix /opt/airymax
```

### Launching

```bash
# Start a single daemon
./build/bin/gateway_d --config gateway_config.json

# Start all daemons via the manager
./daemon_manager --start-all

# Inspect daemon status
./daemon_manager --status
```

### CI/CD Scripts

| Script | Purpose |
|--------|---------|
| `scripts/ci.sh` | CI pipeline build script |
| `scripts/local-ci.sh` | Local CI simulation |
| `scripts/static-analysis.sh` | Static code analysis |
| `scripts/verify-coverage.sh` | Coverage verification |

---

## 6. License

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

This module is dual-licensed under the terms of either:

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt)), or
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

The full license texts are in the [LICENSE](LICENSE) file; the copyright
notice is in [NOTICE](NOTICE). You may select either license to comply with.
The AGPL-3.0-or-later terms apply by default; the Apache-2.0 alternative is
provided for downstream integration scenarios (e.g., closed-source or
proprietary distribution) that the AGPL does not accommodate.
