# daemons — Runtime Daemon Services

> The user-space service layer of the Airymax agent runtime: 15 daemon processes that
> turn the Airymax kernel into a running system, plus the shared `svc_common` library.

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)

- **Repository:** <https://atomgit.com/openairymax/daemons>
- **Version:** 0.1.15
- **License:** AGPL-3.0-or-later OR Apache-2.0

---

## What this is

**daemons** is the service layer of the Airymax agent runtime. It contains **15 long-running
daemon processes** — `gateway_d`, `llm_d`, `tool_d`, `sched_d`, `market_d`, `monit_d`,
`channel_d`, `notify_d`, `hook_d`, `mem_d`, `agent_d`, `a2a_d`, `think_d`, `cupolas_d`,
`maths_d` — and the shared static library `svc_common` (in `common/`).

Each daemon is its own OS process, owns exactly one domain, exposes a JSON-RPC 2.0
interface, and reaches its peers through the IPC service bus. `gateway_d` is the only
process boundary that faces external clients; everything else stays internal.

```
External client ──HTTP / WS / SSE / MCP / A2A / OpenAI API──▶ gateway_d
                                                              │
                                                    JSON-RPC 2.0 over IPC bus
                                                              ▼
                      llm_d  tool_d  sched_d  mem_d  agent_d  …  (14 daemons)
                                                              │
                                                    atoms / syscall ──▶ kernel
```

## Capabilities

- **Service-oriented** — independent processes, IPC cooperation; each daemon can be
  started, scaled, upgraded, and replaced on its own.
- **Single responsibility** — one core domain per daemon, so coupling stays low.
- **Endogenous security** — `svc_common` links `cupolas` as a `PUBLIC` dependency, so
  every daemon inherits request authentication, input sanitization, audit, and sandboxing
  without writing any security code of its own.
- **Unified protocol** — JSON-RPC 2.0 everywhere inside the runtime; MCP / A2A /
  OpenAI-API translation happens only at the gateway boundary.
- **Resilience** — circuit breaker, API recovery with primary/backup failover, health
  checks, and automatic restart of degraded services.
- **Observability** — every daemon reports metrics to `monit_d` and events to `notify_d`,
  and writes a per-process log you can read with `airymaxrt logs <daemon>_d`.
- **Lifecycle framework** — one `airy_svc_t` state machine and one event-driven
  main loop (`daemon_event_driver`) shared by all 15 processes.

## The 15 daemons

| # | Daemon | RPC namespace | Responsibility |
|---|--------|---------------|----------------|
| 1 | [gateway_d](gateway_d/README.md) | — (entry point) | Sole external boundary. Translates HTTP / WebSocket / SSE / MCP / A2A / OpenAI API to JSON-RPC 2.0 and forwards by namespace to the other 14 daemons. Contains no business logic. |
| 2 | [llm_d](llm_d/README.md) | `llm.*` | LLM inference: streaming completion, token counting, cost accounting, response caching. |
| 3 | [tool_d](tool_d/README.md) | `tool.*`, `plugin.*` | Tool and plugin registry, discovery, sandboxed execution, parameter validation, result caching. |
| 4 | [sched_d](sched_d/README.md) | `sched.*` | Task and DAG scheduling, roadmap planning, round-robin / weighted / priority / ML strategies. |
| 5 | [market_d](market_d/README.md) | `market.*` | Agent / Skill / Tool / Template artifacts: search, install, versioning, uninstall. |
| 6 | [monit_d](monit_d/README.md) | `monit.*` | Metrics collection and query, system and hardware info, health checks, alert rules, runaway-agent detection. |
| 7 | [channel_d](channel_d/README.md) | `channel.*` | Data-plane application channels: channel create / join / send / receive and message routing. |
| 8 | [notify_d](notify_d/README.md) | `notify.*` | Event fan-out: topic-based publish / subscribe over WebSocket, SSE, and sockets. |
| 9 | [hook_d](hook_d/README.md) | `hook.*` | Hook and session registration; the hook engine itself lives in `atoms/coreloopthree`. |
| 10 | [mem_d](mem_d/README.md) | `mem.*` | Persistent memory: write / search / get / delete / recent / evolve, hybrid TF-IDF + embedding retrieval, JSONL storage. |
| 11 | [agent_d](agent_d/README.md) | `agent.*` | Agent lifecycle and the execution loop: `run` / `run_stream` / `run_cancel`, spawn / invoke / terminate / cancel. |
| 12 | [a2a_d](a2a_d/README.md) | `a2a.*` | Agent-to-Agent protocol: Agent Card registration and discovery, task state machine, message delivery. |
| 13 | [think_d](think_d/README.md) | `think.*` | Cognition service: two-pass interaction, pipeline orchestration, language front-end, review. |
| 14 | [cupolas_d](cupolas_d/README.md) | `cupolas.*`, `policy.*` | Security policy decision point: permission checks, sanitization, audit, credential vault, network rules, policy load / activate / rollback. |
| 15 | [maths_d](maths_d/README.md) | `maths.*` | Mathematics coprocessor: pure-C numeric and statistical evaluation, plus an optional symbolic backend. |

Executable names keep the `*_d` suffix and match the CMake target names one for one
(`gateway_d`, `llm_d`, …). Each subdirectory has its own README documenting its interface.

## Layout

```
daemons/
├── CMakeLists.txt      # builds the 15 daemons + svc_common
├── common/             # svc_common static library (shared service framework)
├── scripts/            # CI, local verification, static analysis, coverage
├── gateway_d/ … maths_d/   # one directory per daemon
├── Dockerfile.ci       # CI build environment
├── LICENSE             # AGPL-3.0 + Apache-2.0 dual license texts
└── NOTICE              # copyright and core-IP notice
```

Each daemon directory follows the same shape:

```
<name>_d/
├── CMakeLists.txt      # target <name>_d
├── README.md           # responsibilities, RPC interface, dependencies, how to run
├── include/            # public headers
├── src/                # sources (main.c registers the JSON-RPC methods)
└── tests/              # unit tests
```

### svc_common (`common/`)

`common/` builds the `svc_common` static library, which every daemon links `PRIVATE`.
It provides the service framework (`airy_svc_t`, event driver, task dispatcher,
bootstrap for IPC / systemd / Cupolas), the IPC client and service bus, the JSON-RPC
method dispatcher and parameter validators, resilience components (circuit breaker,
API recovery, input validator, log sanitizer), metrics and alerting, configuration,
and the platform compatibility layer. See [`common/README.md`](common/README.md).

## Usage

### Run

The runtime is managed by the `airymaxrt` launcher; it brings the daemon cluster up
and down for you, so you normally never start a daemon by hand.

```bash
airymaxrt                 # terminal UI — starts the runtime and its services
airymaxrt status          # current runtime state
airymaxrt doctor          # component health check
airymaxrt logs 100        # last 100 lines of runtime log
airymaxrt logs llm_d      # log of one daemon
airymaxrt monitor         # continuous observation
```

Other subcommands are `cli`, `profile`, `update`, `uninstall`, `reinstall`.
There is no `airymaxrt start` — running the launcher with no arguments starts everything.

To exercise a single daemon's interface directly, start it and send JSON-RPC to its
socket:

```bash
<build-dir>/bin/maths_d                     # listens on the runtime dir
```

On POSIX the endpoint is a Unix socket `<runtime-dir>/<name>.sock`, resolved from the
runtime root, so a manually started daemon joins the same bus as a launched one. On
Windows the daemon serves the local TCP loopback `127.0.0.1:<port>`; the per-daemon
default port is documented in its README.

### Build from source

Prerequisites: CMake ≥ 3.16, a C11 compiler (GCC / Clang / MSVC), cJSON.
Optional: GTest (unit tests), lcov + genhtml (coverage), cppcheck (static analysis).

```bash
cmake -S . -B ../daemons-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../daemons-build --parallel
```

Keep the build directory outside the source tree.

CMake options:

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_DAEMON` | POSIX `ON`, Windows `OFF` | Build the daemon cluster (set by the parent build) |
| `BUILD_TESTS` | `ON` (forced `OFF` on Windows) | Build unit tests and enable CTest |
| `BUILD_COVERAGE` | `OFF` | Instrument for coverage and add the `coverage` target |
| `BUILD_ALL_PLATFORMS` | `OFF` | Cross-compile for all platforms |

Configuring with `BUILD_DAEMON=OFF` skips this module with a warning. On Windows,
build the daemons and the CLI explicitly:

```bash
cmake -S . -B ../agentrt-build -DBUILD_DAEMON=ON -DBUILD_CLI=ON
```

The official Windows release packages do ship the daemons and the CLI; only a plain
source build leaves them off by default.

Artifacts and installation:

```bash
ctest --test-dir ../daemons-build --output-on-failure
cmake --install ../daemons-build --prefix /opt/airymax   # binaries → <prefix>/bin
```

- 15 daemon executables in `${CMAKE_BINARY_DIR}/bin/`
- `svc_common` static library, consumed privately by each daemon
- Daemon public headers installed under `include/agentrt/`

### CI scripts

| Script | Purpose |
|--------|---------|
| [`scripts/`](scripts/README.md) | CI entry: build, test, cppcheck, coverage |
| `scripts/local-ci.sh` | Local CI simulation |
| `scripts/static-analysis.sh` | cppcheck static analysis |
| `scripts/verify-coverage.sh` | Coverage collection and threshold check |

## Interfaces

**Service lifecycle** — one state machine shared by all daemons
(`airy_svc_state_t`): `NONE → CREATED → INITIALIZING → READY → RUNNING → PAUSED →
STOPPING → STOPPED`, with `ZOMBIE` for stop timeouts and `ERROR` for failures.
Startup order is `init → load config → register with service discovery → serve →
graceful shutdown`.

**Capability flags** — a daemon advertises what it supports through
`airy_svc_config_t.capabilities`: `AIRY_SVC_CAP_NONE / ASYNC / STREAMING / CANCELABLE /
PAUSEABLE / THROTTLE / BATCH / PRIORITY / TIMEOUT`.

**Error codes** — the standard `AIRY_E*` set from `commons`, extended by daemon-level
aliases in `daemon_errors.h`.

```c
#include "svc_common.h"
#include "ipc_service_bus.h"

int main(void)
{
    airy_svc_config_t cfg = {
        .name           = "my_daemon",
        .version        = "0.1.15",
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

## Relationships

daemons is a composition layer: it does not define kernel primitives, it turns them
into running processes.

| Dependency | What daemons uses |
|------------|-------------------|
| [commons](https://atomgit.com/openairymax/commons) | Logging, configuration, networking, tokens, cost, observability, platform paths and the authoritative IPC headers — reached transitively through `svc_common` |
| [atoms](https://atomgit.com/openairymax/atoms) | Syscall entry surface for downward dispatch; `hook_d` links the CoreLoopThree hook library directly |
| [cupolas](https://atomgit.com/openairymax/cupolas) | Security dome, `PUBLIC`-linked by `svc_common`; `cupolas_d` exposes it as a service |
| [protocols](https://atomgit.com/openairymax/protocols) | JSON-RPC 2.0 / AgentsIPC envelope on the IPC bus; A2A and MCP adapters at the gateway |
| [heapstore](https://atomgit.com/openairymax/heapstore) | Persistence for daemon state, registries, and budgets |
| [gateway](https://atomgit.com/openairymax/gateway) | The gateway library that `gateway_d` wraps as a service |

| Consumer | What it uses |
|----------|--------------|
| SDK / Agent applications | The gateway's JSON-RPC 2.0 surface, through daemon client libraries shipped by the SDK |
| Command line and terminal UI | Memory read/write, cognition, status and logs |
| Ecosystem tooling and skills | Daemon services via the SDK |

## Documentation

Design documents, interface references, and application guides live in the
[Airymax documentation repository](https://atomgit.com/openairymax/docs) under
`AirymaxRT/`. Start with `AirymaxRT/README.md`; the API reference is in
`AirymaxRT/30-interfaces/`.

## License

Copyright (c) 2025-2026 SPHARX Ltd.

This module is dual-licensed under either:

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt)), or
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

Full license texts are in [LICENSE](LICENSE); the copyright and core-IP notice is in
[NOTICE](NOTICE). You may choose either license. AGPL-3.0-or-later applies by default;
Apache-2.0 is offered for downstream integration scenarios the AGPL does not accommodate.
