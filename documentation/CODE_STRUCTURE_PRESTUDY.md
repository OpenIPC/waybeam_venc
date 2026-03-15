# Code Structure & Conventions Prestudy

**Date:** 2026-03-11
**Scope:** `star6e-standalone/` — the active implementation

---

## 1. Current State Summary

| File | Lines | Role |
|------|------:|------|
| `backend_star6e.c` | 2452 | Star6E full pipeline: init, stream, teardown, audio, RTP, API callbacks |
| `backend_maruko.c` | 2135 | Maruko full pipeline: same scope, independently reimplemented |
| `sigmastar_types.h` | 1117 | ABI-matching type defs for i6/i6e/i6c platforms |
| `star6e.h` | 318 | MI API wrapper typedefs + function decls + `#ifdef` shim macros |
| `venc_config.c` | 498 | JSON config load/save/defaults |
| `venc_api.c` | 484 | HTTP API field-descriptor engine, route handlers |
| `sensor_select.c` | 389 | Sensor mode scoring, selection, unlock strategy |
| `venc_httpd.c` | 343 | Minimal HTTP/1.0 server |
| `shared.c` | 133 | `printHelp()` + `sendPacket()` (legacy) |
| `main.h` | 114 | Legacy mega-header (HiSilicon includes, macros, arg parser) |
| `main.c` | 76 | Entry point, duplicate-instance check |

**Total project-authored C:** ~8,059 lines (excluding vendored cJSON, SDK, libs).

---

## 2. Reference Conventions for C Projects of This Size

For embedded C projects in the 5–10 KLOC range with multi-backend architecture (like OpenIPC, Majestic, ffmpeg plugins), well-maintained codebases converge on these conventions:

### 2.1 Modularity
- **One concern per file.** A "pipeline" module manages init/teardown; RTP streaming is its own module; audio is its own module; hardware clock/exposure helpers are their own module.
- **Backend abstraction via function-pointer struct** (vtable pattern). Common orchestration code calls through the vtable; backends only implement the hardware-specific parts.
- Shared logic (RTP packetization, start-code stripping, NALU parsing, adaptive payload) lives once in a shared module.

### 2.2 Header Discipline
- Every `.c` file has a matching `.h` with only its public interface.
- No "mega-header" that drags in the entire world — each module includes only what it needs.
- Platform/backend selection via `-D` flags in the build system, not `#ifdef` forests in headers.

### 2.3 Naming
- Consistent prefix per module: `rtp_*`, `venc_pipeline_*`, `audio_*`, `sensor_*`.
- No mixed naming: pick either `snake_case` throughout or `camelCase` throughout (the project already overwhelmingly uses `snake_case` in new code, with `camelCase` artifacts from the legacy HiSilicon era).

### 2.4 Build System
- Makefile lists source files per-backend explicitly (already done — good).
- Shared source files compiled into both backends without duplication.

### 2.5 Plugin/Extension Friendliness
- Backends register themselves through a struct of function pointers, making it straightforward to add a third backend (or a stub/mock for testing).
- Streaming output (RTP, compact, future RTSP) modular — selected at init, not hard-coded in the main loop.

---

## 3. Identified Issues

### TOP 5 — High Impact (Action Now)

#### H1. Massive Backend File Duplication (~60% code overlap)

**Problem:** `backend_star6e.c` (2452 lines) and `backend_maruko.c` (2135 lines) independently reimplement:
- RTP packetization (`send_rtp_packet`, `send_fu_hevc`/`send_fu_h26x`, `send_nal_rtp_hevc`/`send_nal_rtp_h264`)
- Start-code stripping (`strip_start_code` / `maruko_strip_start_code`)
- NALU type extraction (`hevc_get_nalu_type` / `maruko_hevc_nalu_type`)
- Adaptive payload sizing (`adaptive_payload_update` / `maruko_adaptive_payload_update`)
- ISP bin loading (`load_isp_bin` / `maruko_load_isp_bin`)
- File validation (`validate_regular_file` / `maruko_validate_regular_file`)
- `cus3a` enable (`enable_cus3a` / `maruko_enable_cus3a`)
- H.26x attribute filling (`fill_h26x_attr` / `maruko_fill_h26x_attr`)
- Codec/RC resolution (`resolve_codec_rc` / `maruko_resolve_codec_rc`)
- Compact frame sending
- Signal handling (`handle_signal` / `maruko_handle_signal`)
- SDK stdout suppression (`sdk_quiet_*` / duplicated in `sensor_select.c` too)

**Impact:** Every bug fix or feature must be applied twice. Divergence has already crept in (e.g., Star6E has `cus3a_refresh()`/`cus3a_tick()` that Maruko lacks; Maruko uses `i6c_venc_*` types directly while Star6E uses the `MI_VENC_*` wrapper typedefs).

**Recommended fix:** Extract shared logic into dedicated modules:
- `rtp.c` / `rtp.h` — all RTP/compact packetization
- `pipeline_common.c` — ISP bin loading, cus3a, file validation, clock setup, codec resolution
- `signal_handler.c` — unified signal handling

---

#### H2. No Backend Abstraction Layer (Monolithic Entrypoints)

**Problem:** Each backend is a single monolithic function (`star6e_backend_entrypoint()` / `maruko_backend_entrypoint()`) that contains the entire application lifecycle: arg parsing, sensor init, VIF/VPE/VENC pipeline setup, streaming loop, and teardown — all in one function (or a small cluster of tightly coupled statics).

There is no shared pipeline orchestration. `main.c` dispatches to a backend via `#if` and never regains control.

**Impact:** Adding a new backend means copy-pasting 2000+ lines. Testing individual stages (sensor init, encoding, streaming) in isolation is impossible. The streaming loop cannot be reused.

**Recommended fix:** Define a `BackendOps` vtable:
```c
typedef struct {
    const char *name;
    int (*init_pipeline)(VencConfig *cfg, PipelineState *ps);
    void (*stop_pipeline)(PipelineState *ps);
    size_t (*send_frame)(PipelineState *ps, void *stream);
    // ... per-backend hooks
} BackendOps;
```
Shared orchestration in `pipeline.c` calls through this interface. Each backend only implements the hardware-divergent parts.

---

#### H3. Legacy `main.h` Mega-Header Pollutes the Entire Build

**Problem:** `main.h` is a legacy header from the HiSilicon era that:
1. Includes 30+ HiSilicon SDK headers (guarded by `#ifndef PLATFORM_STAR6E`)
2. Defines custom macros with double-underscore reserved identifiers (`__BeginParseConsoleArguments__`, `__OnArgument`, `__ArgValue`, `__ISP_THREAD__`)
3. Declares unrelated functions (`printHelp`, `sendPacket`, `processStream`, MIPI helpers)
4. Defines `SensorType` enum (only used for HiSilicon, never for SigmaStar)
5. Defines `struct RTPHeader` and `MIN` macro (should be in their own headers)

Both backends `#include "main.h"`, which transitively pulls in everything.

**Impact:** Compilation coupling — changing anything in this header triggers a full rebuild. The double-underscore macros are technically undefined behavior per C99 (reserved for the implementation). The arg-parsing macros are brittle and hard to read.

**Recommended fix:** Delete `main.h`. Move `struct RTPHeader` to `rtp.h`. Move `MIN` to a small `util.h` or just use inline definitions. Remove the HiSilicon includes entirely (Star6E never uses them). Remove the arg-parsing macros (both backends already use `VencConfig`-based arg parsing).

---

#### H4. `shared.c` Is a Legacy Dumping Ground

**Problem:** `shared.c` contains only two functions:
1. `printHelp()` — a 83-line help text printer that documents CLI args. But both backends now parse args from `VencConfig` (JSON) — the CLI arg interface is partially vestigial.
2. `sendPacket()` — an RTP fragmenter guarded by `#ifdef PLATFORM_STAR6E`. This is duplicated by the more complete `send_rtp_packet()` / `send_fu_hevc()` in `backend_star6e.c`.

The file includes `main.h` which drags in the entire HiSilicon universe.

**Impact:** `sendPacket()` is dead code for the Maruko backend and duplicated for Star6E. `printHelp()` documents a CLI interface that is increasingly superseded by JSON config + HTTP API.

**Recommended fix:** Move `printHelp()` into a `cli.c` or inline it where needed. Remove `sendPacket()` (the backend-specific RTP code is more complete). Delete `shared.c`.

---

#### H5. Inconsistent Naming Conventions Across the Codebase

**Problem:** The codebase mixes multiple naming conventions:
- **Functions:** `printHelp` (camelCase), `venc_config_load` (snake_case), `maruko_enable_cus3a` (prefixed_snake), `__ISP_THREAD__` (reserved identifier)
- **Types:** `SensorType` (PascalCase), `MarukoBackendConfig` (PascalCase), `i6_venc_chn` (snake_case), `MI_S32` (SCREAMING_CASE vendor types)
- **Macros:** `__BeginParseConsoleArguments__` (reserved), `IDLE_FPS` (standard), `RTP_DEFAULT_PAYLOAD` (standard)
- **Globals:** `g_running` (good), `g_maruko_isp` (good), but `g_apply_venc_chn` vs `g_maruko_verbose_ptr` (mixed prefix conventions)

**Impact:** New contributors (and AI agents) struggle to predict function names. Searching for "the RTP send function" returns different names in each backend. Double-underscore identifiers are technically UB.

**Recommended fix:** Standardize on:
- `snake_case` for all project-authored functions and variables
- `PascalCase` for project-authored typedefs/structs (already mostly done: `VencConfig`, `SensorStrategy`)
- Module prefix for public functions: `rtp_*`, `pipeline_*`, `audio_*`, `sensor_*`
- `g_` prefix for file-scope globals (already mostly done)
- Remove all double-underscore identifiers

---

### MEDIUM/MINOR 5 — Lower Priority (Address Later)

#### M1. SDK Stdout Suppression Duplicated 3 Times

`sdk_quiet_begin()`/`sdk_quiet_end()` in `backend_star6e.c`, `sdk_quiet()` in `sensor_select.c`, and implicit dup/dup2 patterns in `backend_maruko.c` — same logic, three copies.

**Fix:** Single `sdk_quiet.c` / `sdk_quiet.h` shared module.

---

#### M2. Maruko Backend Has Its Own Parallel Type System

`backend_maruko.c` defines `i6c_isp_impl`, `i6c_scl_impl`, `i6c_isp_chn`, `i6c_isp_para`, `i6c_isp_port`, `i6c_scl_port` locally as file-scope structs (lines 96-158). These are essentially a private SDK binding layer mixed into the backend logic.

**Fix:** Move these to `maruko_types.h` or a dedicated `maruko_isp.c` module to separate SDK binding from pipeline logic.

---

#### M3. Constants (`IDLE_FPS`, `RTP_DEFAULT_PAYLOAD`, etc.) Defined in Two Places

Both backends define identical constants: `RTP_DEFAULT_PAYLOAD 1400`, `RTP_BUFFER_MAX 8192`, `RTP_MIN_PAYLOAD 1000`, `ADAPTIVE_HYSTERESIS_NUM 3`, `ADAPTIVE_HYSTERESIS_DEN 20`. Star6E also defines `IDLE_FPS 5`.

**Fix:** Move to a shared header (`rtp.h` for RTP constants, `pipeline.h` for pipeline constants).

---

#### M4. Streaming Output Mode Hardcoded in Backend Loop

The main streaming loop in each backend has `if (stream_mode == RTP) ... else if (stream_mode == COMPACT)` branches inline. Adding a third output mode (e.g., RTSP, file recording) means modifying each backend's main loop.

**Fix:** Abstract output sinks behind a function-pointer interface (`OutputSink` with `send_frame`, `init`, `teardown`). Select sink at pipeline init, not in the hot loop.

---

#### M5. No `const` Discipline on Read-Only Parameters

Many functions take `VencConfig*` where `const VencConfig*` would be correct (e.g., in backend config-read paths). Similarly, several `struct sockaddr*` parameters should be `const`. The field descriptor table in `venc_api.c` casts away const from `g_cfg` for writes.

**Fix:** Add `const` annotations incrementally during refactoring. This catches accidental mutation bugs at compile time and documents intent.

---

## 4. Suggested Refactoring Order

The dependencies between the top-5 fixes suggest this order:

1. **H5 (naming)** — Establish naming convention first, apply to new code going forward
2. **H3 (main.h)** — Remove the legacy mega-header to unblock clean module boundaries
3. **H4 (shared.c)** — Remove the legacy dumping ground
4. **H1 (dedup)** — Extract shared modules (`rtp.c`, `pipeline_common.c`, etc.)
5. **H2 (backend vtable)** — Introduce `BackendOps` abstraction on top of the cleaned modules

Each step should be independently buildable and verifiable (`make verify` after each).
