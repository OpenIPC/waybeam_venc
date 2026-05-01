# Venc Modularization Feasibility Study

**Status:** DEPRECATED. Superseded by a lighter-touch ring buffer approach.
This study remains as reference material for the full library modularization
design, but the implementation will not follow this plan.

**Goal:** Make venc loadable and controllable as a library from a parent
program (e.g. waybeam-hub) instead of only running as a standalone process.

## Executive Summary

Modularizing venc into a library with a `venc_init()` / `venc_start()` /
`venc_stop()` API is **feasible** with moderate effort. The codebase already
has clean boundaries in configuration, HTTP API, and sensor selection. The
main obstacles are: (1) global mutable state in backends used by the HTTP
API callbacks, (2) signal handling owned by venc, and (3) the monolithic
backend entrypoint functions that mix init, streaming, and cleanup in a
single 200-line function.

**Estimated effort:** ~1 week for a minimal library interface, ~2 weeks for
a fully clean context-based API with no global state.

## Current Architecture (What Needs to Change)

### Entry Flow Today

```
main()
  └─ star6e_backend_entrypoint()     ← 200-line monolith
       ├─ install_signal_handlers()  ← owns SIGINT/SIGTERM/SIGHUP
       ├─ venc_config_load()         ← stack-local VencConfig
       ├─ MI_SYS_Init()             ← SDK init
       ├─ venc_httpd_start()         ← spawns HTTP thread
       ├─ pipeline_start()           ← sensor + VIF + VPE + VENC
       ├─ venc_api_register()        ← wires callbacks to globals
       └─ while (g_running) { ... }  ← blocking streaming loop
```

### Global State Inventory

| Variable | File | Purpose | Blocks multi-instance? |
|----------|------|---------|----------------------|
| `g_running` | backend_star6e.c | Signal-driven shutdown flag | Yes |
| `g_signal_count` | backend_star6e.c | Double-SIGINT detection | Yes |
| `g_apply_venc_chn` | backend_star6e.c | VENC channel for API callbacks | Yes |
| `g_apply_vpe_port` | backend_star6e.c | VPE port for API callbacks | Yes |
| `g_apply_venc_port` | backend_star6e.c | VENC port for API callbacks | Yes |
| `g_sensor_fps` | backend_star6e.c | Sensor max FPS for API | Yes |
| `g_devnull_fd` | backend_star6e.c | SDK output suppression | No (stateless) |
| `g_cfg` | venc_api.c | Pointer to live config | Yes |
| `g_cb` | venc_api.c | Backend callback vtable | Yes |
| `g_backend` | venc_api.c | Backend name string | Yes |
| `g_cfg_mutex` | venc_api.c | Config access lock | Yes |
| `g_reinit` | venc_api.c | Reinit request flag | Yes |
| `g_routes[]` | venc_httpd.c | HTTP route table | Yes |
| `g_route_count` | venc_httpd.c | Route count | Yes |
| `g_listen_fd` | venc_httpd.c | HTTP listen socket | Yes |
| `g_thread` | venc_httpd.c | HTTP thread handle | Yes |

**13 of 16** global variables block clean multi-instance or library usage.

### What's Already Clean

These modules need little or no change:

| Module | Why it's clean |
|--------|---------------|
| `venc_config.c` | Stateless. Operates on caller-owned `VencConfig*`. |
| `sensor_select.c` | Pure algorithm. No globals, no SDK state. |
| `venc_config.h` | Data-only structs. No SDK dependencies. |
| `cJSON` | Standalone library. |

## Proposed Library Interface

### Minimal API (libvenc)

```c
/* venc.h — public library interface */

#include "venc_config.h"

typedef struct VencContext VencContext;

/* Lifecycle */
int  venc_init(VencContext **ctx, const VencConfig *cfg);
int  venc_start(VencContext *ctx);
void venc_stop(VencContext *ctx);
void venc_destroy(VencContext *ctx);

/* Runtime control (thread-safe) */
int  venc_set_bitrate(VencContext *ctx, uint32_t kbps);
int  venc_set_fps(VencContext *ctx, uint32_t fps);
int  venc_set_gop(VencContext *ctx, uint32_t gop_size);
int  venc_request_idr(VencContext *ctx);
int  venc_request_reinit(VencContext *ctx, const VencConfig *new_cfg);

/* Frame access (alternative to built-in UDP/RTP streaming) */
typedef void (*VencFrameCallback)(const uint8_t *data, size_t len,
                                  bool is_keyframe, void *user_data);
int  venc_set_frame_callback(VencContext *ctx, VencFrameCallback cb,
                             void *user_data);

/* Status */
int  venc_is_running(const VencContext *ctx);
int  venc_get_config(const VencContext *ctx, VencConfig *out);
```

### VencContext (Opaque)

All current global state moves into this struct:

```c
struct VencContext {
    VencConfig          cfg;
    PipelineState       pipeline;

    /* Streaming */
    volatile int        running;
    pthread_t           stream_thread;

    /* API / control */
    VencApplyCallbacks  callbacks;
    pthread_mutex_t     cfg_mutex;
    volatile int        reinit_pending;

    /* HTTP (optional, can be disabled for embedded use) */
    int                 httpd_enabled;
    int                 httpd_listen_fd;
    pthread_t           httpd_thread;

    /* Frame callback (for parent program integration) */
    VencFrameCallback   frame_cb;
    void               *frame_cb_user;

    /* SDK handles */
    MI_VENC_CHN         venc_chn;
    MI_SYS_ChnPort_t    vpe_port;
    MI_SYS_ChnPort_t    venc_port;
    uint32_t            sensor_fps;
};
```

### Usage from waybeam-hub

```c
#include "venc.h"

void on_frame(const uint8_t *data, size_t len, bool keyframe, void *ud) {
    waybeam_hub_t *hub = ud;
    waybeam_hub_forward_video(hub, data, len, keyframe);
}

int start_encoder(waybeam_hub_t *hub) {
    VencConfig cfg;
    venc_config_defaults(&cfg);
    venc_config_load("/etc/venc.json", &cfg);

    VencContext *ctx;
    if (venc_init(&ctx, &cfg) != 0)
        return -1;

    /* Receive frames directly instead of UDP streaming */
    venc_set_frame_callback(ctx, on_frame, hub);

    /* Disable built-in HTTP API (waybeam-hub has its own) */
    // cfg.system.web_port = 0;  // or a flag in venc_init options

    return venc_start(ctx);  /* non-blocking: spawns streaming thread */
}

void stop_encoder(VencContext *ctx) {
    venc_stop(ctx);
    venc_destroy(ctx);
}
```

## Implementation Plan

### Phase 1: Context Struct (Minimal Refactor)

Move global state into a `VencContext` struct. Pass it through all
internal functions. This is the largest change but is mostly mechanical.

**Files changed:**
- `backend_star6e.c` — replace all `g_apply_*` globals with context members
- `backend_maruko.c` — same treatment
- `venc_api.c` — replace `g_cfg`, `g_cb`, `g_reinit` with context pointer
- `venc_httpd.c` — replace `g_routes[]`, `g_listen_fd` with context members

**Risk:** Low. Mechanical refactor, testable incrementally.

### Phase 2: Split Backend Entrypoint

Break `star6e_backend_entrypoint()` into discrete functions:

```
star6e_backend_entrypoint()
  → venc_init()        — config load, SDK init, pipeline start
  → venc_run_loop()    — streaming loop (can run in caller thread or spawned)
  → venc_cleanup()     — pipeline stop, SDK exit
```

**Files changed:**
- `backend_star6e.c` — extract init/loop/cleanup
- `backend_maruko.c` — same

**Risk:** Low. The streaming loop is already a clean `while` block.

### Phase 3: Frame Callback Interface

Add `VencFrameCallback` as an alternative to the built-in UDP/RTP send.
When a callback is registered, `send_frame_rtp()` / `send_frame_compact()`
are bypassed and raw NALUs are delivered to the callback instead.

**Files changed:**
- `backend_star6e.c` — add callback invocation path in streaming loop
- `backend_maruko.c` — same
- New: `include/venc.h` — public API header

**Risk:** Low. Additive change, does not affect existing streaming path.

### Phase 4: Signal Handler Decoupling

Remove venc's ownership of SIGINT/SIGTERM/SIGHUP. Instead:
- `venc_stop()` is the shutdown mechanism (called by parent)
- Reinit is triggered via `venc_request_reinit()` (called by parent)
- The standalone `main.c` installs signal handlers that call these functions

**Files changed:**
- `backend_star6e.c` — remove `install_signal_handlers()` from init path
- `main.c` — install handlers that call `venc_stop()` / `venc_request_reinit()`

**Risk:** Medium. Signal handler timing and the 5-second alarm watchdog
need careful handling. The parent program must guarantee it calls
`venc_stop()` on shutdown.

### Phase 5: Optional HTTP API

Make the built-in HTTP API optional. When `web_port == 0` or a flag is set,
`venc_httpd_start()` is skipped. The parent program can expose its own
control interface and call `venc_set_*()` functions directly.

**Files changed:**
- `backend_star6e.c` — conditional httpd start
- `venc_api.c` — no change (routes just won't be registered)

**Risk:** Low. Already nearly optional.

### Phase 6: Build as Library

Add a `make libvenc` target that produces `libvenc.a` (static) or
`libvenc.so` (shared). The existing `make build` continues to produce the
standalone binary by linking `main.c` against `libvenc.a`.

**Makefile changes:**
- New target: `libvenc.a` — compile all .o except main.o, archive
- Existing `venc` target: link `main.o` + `libvenc.a`
- Install target: copy `libvenc.a` + `venc.h` + `venc_config.h`

**Risk:** Low. Standard library packaging.

## Key Design Decisions

### Single-Instance Constraint

The SigmaStar SDK (`MI_SYS_Init()`, `MI_VENC_CreateChn()`, etc.) assumes
a single user per hardware pipeline. Running two VencContexts
simultaneously on the same SoC is **not supported by hardware**. The
context struct enables clean lifecycle management, not multi-instance.

The `is_another_venc_running()` check moves to the standalone `main()` and
is not part of the library — the parent program manages its own process
exclusivity.

### Streaming Thread Ownership

Two options:

| Option | Pros | Cons |
|--------|------|------|
| **A: Library spawns thread** | Simple API. `venc_start()` returns immediately. | Parent can't control thread priority/affinity. |
| **B: Caller drives loop** | Full control. `venc_poll()` called from parent's event loop. | Requires tight polling (~1ms). More complex API. |

**Recommendation:** Option A (library spawns thread) with an escape hatch:
`venc_start_sync(ctx)` for callers who want to run the loop in their own
thread.

### Frame Delivery Mechanism

When a parent program (waybeam-hub) wants to receive encoded frames
directly instead of via UDP:

| Mechanism | Latency | Complexity |
|-----------|---------|------------|
| **Callback** | Lowest (~0 overhead) | Low. Called from streaming thread. |
| **Ring buffer** | Low (memcpy) | Medium. Needs size management. |
| **Unix socket** | Medium (syscall) | Medium. Familiar IPC. |
| **Shared memory** | Lowest | High. Synchronization complexity. |

**Recommendation:** Callback (Phase 3). It's the simplest, has zero copy
overhead, and the streaming thread already has the data. The callback runs
in the streaming thread context — if the parent needs async delivery, it
can enqueue internally.

### HTTP API Coexistence

When waybeam-hub embeds venc, it likely has its own HTTP/WebSocket server.
Two options:

1. **Disable venc's httpd entirely.** Parent calls `venc_set_*()` directly.
2. **Keep venc's httpd on a separate port.** Useful for debugging.

Both should be supported. `web_port = 0` disables. Any nonzero port enables
the built-in HTTP API alongside the parent's control path.

## Dependency Impact on waybeam-hub

### What waybeam-hub Must Link Against

```
libvenc.a (or .so)
  ├─ libmi_vif.so      ─┐
  ├─ libmi_vpe.so       │
  ├─ libmi_venc.so      ├─ SigmaStar SDK (already on target)
  ├─ libmi_isp.so       │
  ├─ libmi_sensor.so    │
  ├─ libmi_sys.so       │
  ├─ libcus3a.so        │
  ├─ libispalgo.so     ─┘
  └─ libpthread         ← standard
```

No new dependencies. waybeam-hub already runs on the same SoC and likely
already links some of these.

### Header Exposure

The public API exposes only:
- `venc.h` — lifecycle + control functions
- `venc_config.h` — config structs (pure C, no SDK types)

SDK headers (`mi_*.h`, `star6e.h`, `sigmastar_types.h`) remain internal.
The `VencContext` is opaque — parent code never sees SDK types.

## What Does NOT Need to Change

| Component | Reason |
|-----------|--------|
| `venc_config.c/h` | Already stateless, caller-owned structs |
| `sensor_select.c/h` | Pure algorithm, no globals |
| `cJSON` | Standalone library |
| SDK interaction patterns | MI_* call sequences stay the same |
| RTP/UDP streaming | Stays as default; callback is additive |
| Pipeline init order | VIF→VPE→VENC chain unchanged |
| Reinit logic | Same teardown/rebuild, just triggered differently |

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| SDK assumes single caller | Known | High | Document single-instance. Library enforces via init guard. |
| Signal handler conflicts | Medium | Medium | Phase 4: parent owns signals, library exposes `venc_stop()`. |
| Thread safety regressions | Medium | Medium | Mutex already exists. Context struct makes ownership explicit. |
| Callback blocks streaming | Low | High | Document: callback must return quickly. Add timeout warning. |
| Build system complexity | Low | Low | Two targets (binary + library) sharing same objects. |

## Migration Path

The refactoring can be done incrementally without breaking the standalone
binary at any point:

1. **Phase 1-2:** Internal refactor only. `main.c` calls the same
   entrypoint, which internally uses the new context struct. No external
   API change. Existing behavior preserved.

2. **Phase 3-5:** Add `venc.h` public API. `main.c` becomes a thin wrapper
   that calls `venc_init()` / `venc_start()` / `venc_stop()`. This is the
   "proof" that the library interface works.

3. **Phase 6:** Add `make libvenc` target. waybeam-hub can start
   integrating.

At every phase, `make verify` continues to pass. The standalone binary
remains the primary deliverable until waybeam-hub integration is validated.

## Alternatives Considered

### IPC Instead of Library Embedding

Run venc as a separate process, communicate via Unix socket or shared
memory.

| | Library | IPC (separate process) |
|-|---------|----------------------|
| Latency | ~0 (function call) | ~50-100μs (syscall + context switch) |
| Complexity | Moderate refactor | New protocol design |
| Crash isolation | Shared fate | Independent (pro and con) |
| Memory overhead | Shared address space | Separate heap (~2MB extra) |
| Resource control | Parent controls everything | Separate scheduling |

**Verdict:** Library embedding is better for the use case. The encoder and
parent are tightly coupled (same SoC, same pipeline), crash isolation is
not a priority (if the encoder crashes, the whole video path is dead
anyway), and zero-copy frame delivery matters for latency.

IPC makes sense only if waybeam-hub needs to survive encoder crashes and
restart it independently — which the current `MI_SYS_Exit()` + reinit
flow already handles internally.

### Keep as Subprocess, Control via HTTP API

waybeam-hub spawns `venc` as a child process and controls it via the
existing HTTP API on localhost.

**Pros:** Zero code changes to venc. Already works today.
**Cons:** No direct frame access (must receive via UDP loopback or shared
memory). HTTP overhead for control commands. Process management complexity.

**Verdict:** Good as an interim solution. The HTTP API already supports
all runtime controls. waybeam-hub could start with this approach
immediately and migrate to library embedding later.

## Recommendation

1. **Immediate (no code changes):** waybeam-hub spawns venc as a subprocess
   and controls it via HTTP API + receives video via UDP loopback. This
   works today.

2. **Short-term (Phase 1-3, ~1 week):** Context struct + split entrypoint +
   frame callback. This gives waybeam-hub direct frame access with minimal
   API surface.

3. **Full integration (Phase 1-6, ~2 weeks):** Complete library with clean
   signal handling, optional HTTP, and proper build targets. This is the
   end state for tight integration.

---

## Detailed Implementation Plan

This section provides step-by-step implementation instructions with exact
file/line references, diffs, and verification steps between each phase.

### Phase 1: Context Struct — Eliminate Global State

**Goal:** Move all instance-specific globals into a `VencContext` struct.
No API change, no behavior change. The standalone binary works identically.

#### Step 1.1: Define VencContext (new internal header)

Create `include/venc_internal.h`:

```c
#ifndef VENC_INTERNAL_H
#define VENC_INTERNAL_H

#include "venc_config.h"
#include "venc_api.h"
#include <pthread.h>
#include <signal.h>
#include <stdint.h>

/* Forward declaration — full definition varies by backend */
typedef struct PipelineState PipelineState;

typedef struct VencContext {
    VencConfig          cfg;

    /* Lifecycle */
    volatile sig_atomic_t running;
    volatile sig_atomic_t signal_count;

    /* API callback state (currently g_apply_* globals in backend) */
    VencApplyCallbacks  apply_cb;
    uint32_t            sensor_fps;

    /* API module state (currently globals in venc_api.c) */
    const VencApplyCallbacks *api_cb;
    pthread_mutex_t     cfg_mutex;
    volatile sig_atomic_t reinit;
    char                backend_name[32];

    /* HTTPD state (currently globals in venc_httpd.c) */
    int                 httpd_listen_fd;
    pthread_t           httpd_thread;
    volatile int        httpd_running;

    /* Frame callback (Phase 3, NULL until then) */
    void               *frame_cb;
    void               *frame_cb_user;

    /* RTP packet callback (Phase 3, NULL until then) —
     * fires from send_rtp_packet() for wfb-ng integration */
    void               *rtp_cb;
    void               *rtp_cb_user;
} VencContext;

#endif
```

#### Step 1.2: Refactor venc_api.c — replace globals with context pointer

Current globals to replace:

| Global | Becomes |
|--------|---------|
| `g_cfg` | `ctx->cfg` (direct member, not pointer) |
| `g_cb` | `ctx->api_cb` |
| `g_backend[32]` | `ctx->backend_name[32]` |
| `g_cfg_mutex` | `ctx->cfg_mutex` |
| `g_reinit` | `ctx->reinit` |

**API signature change:**

```c
/* Before */
int venc_api_register(VencConfig *cfg, const char *backend_name,
    const VencApplyCallbacks *cb);
void venc_api_request_reinit(int mode);
int  venc_api_get_reinit(void);
void venc_api_clear_reinit(void);

/* After */
int venc_api_register(VencContext *ctx, const char *backend_name,
    const VencApplyCallbacks *cb);
void venc_api_request_reinit(VencContext *ctx, int mode);
int  venc_api_get_reinit(VencContext *ctx);
void venc_api_clear_reinit(VencContext *ctx);
```

**Critical detail:** The route handlers (`handle_set`, `handle_get`, etc.)
currently access globals directly. After refactor, they receive `ctx` via
the `void *ctx` parameter that `venc_httpd_route()` already passes through
to handlers. This is already wired but currently unused (`(void)ctx`).

Changes per handler:
- `handle_version`: access `ctx->backend_name` instead of `g_backend`
- `handle_config`: access `&ctx->cfg` instead of `g_cfg`, lock `ctx->cfg_mutex`
- `handle_set`: access `&ctx->cfg` + `ctx->api_cb`, lock `ctx->cfg_mutex`
- `handle_get`: access `&ctx->cfg`, lock `ctx->cfg_mutex`
- `handle_restart`: call `venc_api_request_reinit(ctx, 1)`
- `handle_idr`: access `ctx->api_cb`

The `field_to_json_value` and `field_from_string` helpers currently use
`g_cfg` directly. Change them to take an explicit `VencConfig *cfg`
parameter.

#### Step 1.3: Refactor venc_httpd.c — replace globals with context pointer

Current globals to replace:

| Global | Becomes |
|--------|---------|
| `g_routes[HTTPD_MAX_ROUTES]` | Stays static (route table is process-wide) |
| `g_route_count` | Stays static |
| `g_listen_fd` | `ctx->httpd_listen_fd` |
| `g_thread` | `ctx->httpd_thread` |
| `g_running` | `ctx->httpd_running` |

**Design note:** The route table (`g_routes[]`) can remain static for now.
Routes are registered once and shared. The httpd thread state (socket, thread
handle) moves into `VencContext` because shutdown must be per-context.

**API signature change:**

```c
/* Before */
int  venc_httpd_start(uint16_t port);
void venc_httpd_stop(void);

/* After */
int  venc_httpd_start(VencContext *ctx, uint16_t port);
void venc_httpd_stop(VencContext *ctx);
```

The `httpd_thread` function receives `ctx` via `pthread_create` arg instead
of `NULL`.

#### Step 1.4: Refactor backend_star6e.c — replace globals with context pointer

Globals to replace:

| Global | Becomes |
|--------|---------|
| `g_running` | `ctx->running` |
| `g_signal_count` | `ctx->signal_count` |
| `g_apply_venc_chn` | Stored in `PipelineState` (already there as `ps.venc_channel`) |
| `g_apply_vpe_port` | Stored in `PipelineState` (already there as `ps.vpe_port`) |
| `g_apply_venc_port` | Stored in `PipelineState` (already there as `ps.venc_port`) |
| `g_sensor_fps` | `ctx->sensor_fps` |

**Critical detail:** The apply callbacks (`apply_bitrate`, `apply_fps`, etc.)
currently access `g_apply_venc_chn` and `g_apply_vpe_port` directly. These
must become closures over the context. Since C has no closures, two options:

**Option A (recommended):** Keep a single static `VencContext *g_ctx` pointer
in the backend file. The apply callbacks use it. This is a stepping stone —
it's still a global, but a single one instead of six, and Phase 2 eliminates
it by restructuring the callback mechanism.

**Option B:** Change `VencApplyCallbacks` function signatures to take a
`void *user_data` parameter. This is cleaner but touches the callback
interface, venc_api.c dispatch code, and both backends. Defer to Phase 2.

**Recommended approach for Phase 1:** Option A. Replace six globals with one
`g_ctx`. Clean and sufficient for single-instance. Phase 2 can optionally
add user_data to callbacks.

```c
/* backend_star6e.c — Phase 1 */
static VencContext *g_ctx;  /* single remaining global, set in entrypoint */

static int apply_bitrate(uint32_t kbps) {
    MI_VENC_CHN chn = g_ctx->pipeline.venc_channel;  /* was g_apply_venc_chn */
    /* ... rest unchanged ... */
}
```

Where `g_ctx->pipeline` is a `PipelineState` member added to `VencContext`.

#### Step 1.5: Refactor backend_maruko.c — same treatment

Same pattern as star6e. Replace `g_maruko_apply_venc_dev`,
`g_maruko_apply_venc_chn`, `g_maruko_verbose_ptr` with `g_ctx` access.

#### Verification: Phase 1

```
make clean && make build SOC_BUILD=star6e    # builds, no warnings
make clean && make build SOC_BUILD=maruko    # builds, no warnings
make test                                     # unit tests pass
make verify                                   # both backends build
```

**Behavioral test:** Deploy to target, run venc, verify:
- HTTP API GET /api/v1/config returns correct config
- HTTP API GET /api/v1/set?video0.bitrate=3000 applies live
- SIGHUP triggers reinit
- SIGINT triggers clean shutdown

---

### Phase 2: Split Backend Entrypoint

**Goal:** Break the monolithic `star6e_backend_entrypoint()` into discrete
init/run/cleanup functions that can be called independently.

#### Step 2.1: Extract init function

Split `star6e_backend_entrypoint()` (lines 1477-1682) into:

```c
/* New: initialize SDK, pipeline, and optionally httpd.
 * Populates ctx with all runtime state.
 * Returns 0 on success, nonzero on failure (ctx partially cleaned up). */
int star6e_init(VencContext *ctx, const VencConfig *cfg);
```

This function performs lines 1477-1512 of the current entrypoint:
1. Copy config into `ctx->cfg`
2. `sdk_quiet_init()` + `MI_SYS_Init()`
3. `venc_httpd_start(ctx, cfg->system.web_port)` (if port != 0)
4. `pipeline_start(&ctx->cfg, &ctx->pipeline)`
5. Set `ctx->sensor_fps`, register API routes

#### Step 2.2: Extract streaming loop

```c
/* New: run the streaming loop. Blocks until ctx->running becomes 0
 * or an unrecoverable error occurs.
 * Handles reinit cycles internally.
 * Returns 0 on clean shutdown, nonzero on error. */
int star6e_run(VencContext *ctx);
```

This function contains the `while (ctx->running)` loop (lines 1528-1671),
including the reinit check and packet streaming logic.

#### Step 2.3: Extract cleanup function

```c
/* New: tear down pipeline, stop httpd, exit SDK.
 * Safe to call even if init partially failed. */
void star6e_cleanup(VencContext *ctx);
```

This function performs lines 1673-1681:
1. `alarm(0)`
2. `pipeline_stop(&ctx->pipeline)`
3. `venc_httpd_stop(ctx)`
4. `MI_SYS_Exit()`
5. `vpe_scl_preset_shutdown()`

#### Step 2.4: Rewrite entrypoint as thin wrapper

```c
int star6e_backend_entrypoint(void) {
    install_signal_handlers();

    VencContext ctx = {0};
    VencConfig cfg;
    venc_config_defaults(&cfg);
    if (venc_config_load(VENC_CONFIG_DEFAULT_PATH, &cfg) != 0)
        return 1;

    int ret = star6e_init(&ctx, &cfg);
    if (ret != 0) {
        star6e_cleanup(&ctx);
        return ret;
    }

    install_signal_handlers();  /* restore after SDK init */
    ret = star6e_run(&ctx);
    star6e_cleanup(&ctx);
    return ret;
}
```

#### Step 2.5: Same for backend_maruko.c

Extract `maruko_init()`, `maruko_run()`, `maruko_cleanup()`.
Rewrite `maruko_backend_entrypoint()` as thin wrapper.

#### Verification: Phase 2

```
make clean && make verify       # both backends build
make test                       # unit tests pass
```

**Behavioral test:** Identical to Phase 1 — no behavior change. Same
deploy-and-test sequence. Additionally verify reinit cycles work (the
reinit logic is now inside `star6e_run()` instead of the monolith).

---

### Phase 3: Frame Callback Interface

**Goal:** Allow a parent program to receive encoded frames directly via
callback instead of (or in addition to) the built-in UDP/RTP sender.

#### Step 3.1: Create public header `include/venc.h`

```c
#ifndef VENC_H
#define VENC_H

#include "venc_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VencContext VencContext;

/* Frame metadata passed to callbacks */
typedef struct {
    const uint8_t *data;       /* NAL unit data (valid only during callback) */
    size_t         len;        /* NAL unit length in bytes */
    bool           is_keyframe;
    uint32_t       pts;        /* presentation timestamp (90kHz clock) */
} VencFrame;

/* Callback invoked from the streaming thread for each encoded NAL unit.
 * MUST return quickly — blocking here stalls the encoder pipeline.
 * The data pointer is invalid after the callback returns. */
typedef void (*VencFrameCallback)(const VencFrame *frame, void *user_data);

/* Lifecycle */
int  venc_init(VencContext **ctx, const VencConfig *cfg);
int  venc_start(VencContext *ctx);
int  venc_start_sync(VencContext *ctx);  /* blocks, runs loop in caller thread */
void venc_stop(VencContext *ctx);
void venc_destroy(VencContext *ctx);

/* Frame callback — set before venc_start(). NULL disables.
 * When set, raw NAL units are delivered via callback. Built-in UDP/RTP
 * sending continues unless outgoing.server is empty. */
int  venc_set_frame_callback(VencContext *ctx, VencFrameCallback cb,
                             void *user_data);

/* RTP packet metadata */
typedef struct {
    const uint8_t *header;     /* 12-byte RTP header */
    const uint8_t *payload;    /* payload (may point into SDK DMA buffer) */
    size_t         payload_len;
    bool           marker;     /* RTP marker bit */
} VencRtpPacket;

/* RTP packet callback — fires for each assembled RTP packet.
 * Designed for wfb-ng integration: delivers the same RTP stream that
 * send_rtp_packet() would send via UDP, enabling zero-copy passthrough
 * to wfb-ng's FEC + encryption pipeline via wfb_tx_send_iov(). */
typedef void (*VencRtpCallback)(const VencRtpPacket *pkt, void *user_data);

int  venc_set_rtp_callback(VencContext *ctx, VencRtpCallback cb,
                           void *user_data);

/* Runtime control (thread-safe, callable from any thread) */
int  venc_set_bitrate(VencContext *ctx, uint32_t kbps);
int  venc_set_fps(VencContext *ctx, uint32_t fps);
int  venc_set_gop(VencContext *ctx, uint32_t gop_size);
int  venc_request_idr(VencContext *ctx);
int  venc_reinit(VencContext *ctx, const VencConfig *new_cfg);

/* Status */
int  venc_is_running(const VencContext *ctx);
int  venc_get_config(const VencContext *ctx, VencConfig *out);

#ifdef __cplusplus
}
#endif

#endif /* VENC_H */
```

#### Step 3.2: Add callback invocation to streaming loop

In `star6e_run()` (extracted in Phase 2), after `MI_VENC_GetStream()` and
before/alongside the existing `send_frame_rtp()`/`send_frame_compact()`:

```c
/* Deliver frames to callback if registered */
if (ctx->frame_cb) {
    VencFrameCallback cb = (VencFrameCallback)ctx->frame_cb;
    for (uint32_t i = 0; i < stream.count; i++) {
        MI_VENC_Pack_t *pack = &stream.packet[i];
        VencFrame frame = {
            .data = pack->data + pack->offset,
            .len  = pack->length - pack->offset,
            .is_keyframe = (pack->type.h265 == I6_H265NALU_ISLICE ||
                           pack->type.h265 == I6_H265NALU_IDRSLICE),
            .pts  = pack->pts,
        };
        cb(&frame, ctx->frame_cb_user);
    }
}

/* Continue with existing UDP/RTP send (or skip if no server configured) */
size_t total_bytes = 0;
if (ps->socket_handle >= 0) {
    total_bytes = (ps->stream_mode == STREAM_MODE_RTP)
        ? send_frame_rtp(...)
        : send_frame_compact(...);
}
```

**Key detail:** The callback receives data from the SDK DMA buffer. The
pointer is only valid during the callback — this is documented and matches
the SDK's `MI_VENC_ReleaseStream()` lifecycle.

#### Step 3.3: Implement venc_start() as threaded wrapper

```c
static void *venc_stream_thread(void *arg) {
    VencContext *ctx = arg;
    ctx->stream_result = star6e_run(ctx);  /* or maruko_run() */
    return NULL;
}

int venc_start(VencContext *ctx) {
    if (pthread_create(&ctx->stream_thread, NULL, venc_stream_thread, ctx) != 0)
        return -1;
    return 0;
}

int venc_start_sync(VencContext *ctx) {
    return star6e_run(ctx);  /* blocks caller */
}
```

#### Step 3.4: WFB-NG compatibility consideration

The frame callback interface must account for the wfb-ng TX integration.
The wfb-ng study proposes an
**RTP passthrough** architecture: standard RTP packets (including H.265 FU
fragmentation, VPS/SPS/PPS injection) are passed directly through wfb-ng's
FEC + encryption pipeline. The wfb-ng RX peer delivers standard RTP to the
ground station decoder — no custom framing needed.

**The key design tension:** The `VencFrameCallback` delivers raw NAL units
from the SDK DMA buffer. But wfb-ng expects **complete RTP packets** — the
output of venc's `send_frame_rtp()` pipeline including start code stripping,
FU fragmentation, and RTP header assembly. If waybeam-hub used the NAL
callback to feed wfb-ng, it would need to duplicate all of venc's RTP
encapsulation logic.

**Solution: RTP packet callback.** Add a second callback type that fires
at the RTP packet level — the same point where `send_rtp_packet()` currently
calls `sendmsg()`:

```c
/* Delivered for each assembled RTP packet (header + payload).
 * This is the natural integration point for wfb-ng: the RTP stream
 * is preserved end-to-end. */
typedef struct {
    const uint8_t *header;     /* 12-byte RTP header (stack buffer) */
    const uint8_t *payload;    /* payload (points into SDK DMA buffer) */
    size_t         payload_len;
    bool           marker;     /* RTP marker bit (last packet of frame) */
} VencRtpPacket;

typedef void (*VencRtpCallback)(const VencRtpPacket *pkt, void *user_data);

int venc_set_rtp_callback(VencContext *ctx, VencRtpCallback cb,
                          void *user_data);
```

This maps directly to the wfb-ng shim's `wfb_tx_send_iov()`:

```c
/* waybeam-hub using both venc + wfb-ng: */
static void on_rtp_packet(const VencRtpPacket *pkt, void *ud) {
    wfb_tx_t *tx = ud;
    /* Feed RTP packet directly into FEC + encrypt + inject.
     * Uses iov variant: 1 copy into FEC block, same as direct. */
    wfb_tx_send_iov(tx, pkt->header, 12,
                    pkt->payload, pkt->payload_len, 0);
}

venc_set_rtp_callback(ctx, on_rtp_packet, hub.tx);
```

**Two callback levels serve different use cases:**

| Callback | Delivers | Use case |
|----------|----------|----------|
| `VencFrameCallback` | Raw NAL units | Recording, analysis, re-encoding |
| `VencRtpCallback` | RTP packets | wfb-ng TX, network forwarding |

Both callbacks can be active simultaneously. The RTP callback fires from
inside `send_rtp_packet()` (the single choke point identified in the wfb-ng
study), while the NAL callback fires earlier in the pipeline.

**Implementation in `send_rtp_packet()`:**

```c
static int send_rtp_packet(...) {
    uint8_t header[12];
    /* ... build RTP header as today ... */

    /* RTP callback (for wfb-ng integration) */
    if (ctx->rtp_cb) {
        VencRtpPacket pkt = {
            .header = header,
            .payload = payload,
            .payload_len = payload_len,
            .marker = marker,
        };
        ((VencRtpCallback)ctx->rtp_cb)(&pkt, ctx->rtp_cb_user);
    }

    /* UDP send (existing path, skipped if no socket) */
    if (socket_handle >= 0) {
        /* sendmsg as today */
    }

    rtp->seq++;
    return 0;
}
```

This eliminates the `STREAM_MODE_WFB` enum from the wfb-ng study. Instead
of wiring wfb-ng into the backend streaming loop, the parent program
connects them via the RTP callback. Benefits:

- **venc stays backend-agnostic** — no `HAVE_WFB` compile flag needed
- **wfb-ng stays independent** — no venc-specific patches
- **waybeam-hub orchestrates** — controls both lifecycles
- **Same zero-copy path** — RTP callback delivers SDK DMA buffer pointer +
  stack-allocated RTP header, passes to `wfb_tx_send_iov()` which copies
  into FEC block (1 copy, same as direct integration)
- **RTP encapsulation reused** — FU fragmentation, VPS/SPS/PPS injection,
  timestamp/sequence management all stay inside venc

```
MI_VENC_GetStream()
    │
    ▼
send_frame_rtp()  ← NAL iteration, FU fragmentation (unchanged)
    │
    ▼
send_rtp_packet()
    ├── VencRtpCallback (in waybeam-hub)
    │    └── wfb_tx_send_iov() → FEC → encrypt → radio
    ├── sendmsg() → UDP (if socket open)
    └── VencFrameCallback (raw NALs, earlier in pipeline)
    │
    ▼
MI_VENC_ReleaseStream()
```

#### Verification: Phase 3

```
make clean && make verify       # builds
make test                       # unit tests pass
```

**New test:** Write a minimal test program that uses the library API:

```c
/* test_libvenc_api.c — compile-time API verification */
#include "venc.h"

static void dummy_cb(const VencFrame *f, void *ud) { (void)f; (void)ud; }

int main(void) {
    VencConfig cfg;
    venc_config_defaults(&cfg);

    VencContext *ctx;
    /* Don't actually call venc_init on x86 — SDK not available.
     * This test verifies headers compile and link correctly. */
    (void)ctx;
    (void)dummy_cb;
    return 0;
}
```

Add to `make test` as a header-compilation check.

**Behavioral test on target:**
1. Run standalone venc — verify existing UDP/RTP streaming works unchanged
2. Write a test harness that uses `venc_init()` + `venc_set_frame_callback()`
   + `venc_start()`, counts frames for 5 seconds, calls `venc_stop()` +
   `venc_destroy()`. Verify frame count matches expected FPS.

---

### Phase 4: Signal Handler Decoupling

**Goal:** Library does not install signal handlers. The standalone binary
installs its own handlers that delegate to `venc_stop()` / `venc_reinit()`.

#### Step 4.1: Remove signal handlers from backend init

In `star6e_init()` and `maruko_init()`: remove the call to
`install_signal_handlers()`. The library's `VencContext.running` flag
starts at 1 (set by `venc_init()`), and is cleared by `venc_stop()`.

#### Step 4.2: Move signal handling to main.c

```c
/* main.c */
#include "venc.h"
#include <signal.h>

static VencContext *g_main_ctx;

static void handle_signal(int sig) {
    if (sig == SIGALRM) {
        _exit(128 + SIGINT);
    }
    if (sig == SIGHUP) {
        venc_reinit(g_main_ctx, NULL);  /* NULL = reload from disk */
        return;
    }
    venc_stop(g_main_ctx);
    if (/* second signal */)
        alarm(5);
}

int main(int argc, char *argv[]) {
    /* ... existing instance check ... */

    VencConfig cfg;
    venc_config_defaults(&cfg);
    venc_config_load(VENC_CONFIG_DEFAULT_PATH, &cfg);

    VencContext *ctx;
    if (venc_init(&ctx, &cfg) != 0)
        return 1;

    g_main_ctx = ctx;
    /* Install signal handlers AFTER init (SDK may clobber them) */
    struct sigaction sa = { .sa_handler = handle_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    int ret = venc_start_sync(ctx);  /* blocking */
    venc_destroy(ctx);
    return ret;
}
```

#### Step 4.3: Add venc_stop() and venc_reinit() implementations

```c
void venc_stop(VencContext *ctx) {
    ctx->running = 0;
}

int venc_reinit(VencContext *ctx, const VencConfig *new_cfg) {
    if (new_cfg) {
        pthread_mutex_lock(&ctx->cfg_mutex);
        ctx->cfg = *new_cfg;
        pthread_mutex_unlock(&ctx->cfg_mutex);
        ctx->reinit = 2;  /* in-memory config */
    } else {
        ctx->reinit = 1;  /* reload from disk */
    }
    return 0;
}
```

#### Step 4.4: Handle the 5-second alarm watchdog

The alarm watchdog (force-exit after SIGINT if cleanup hangs) moves to
`main.c`. The library's `venc_stop()` only sets the flag. If the parent
wants a timeout, it manages it (alarm, or pthread timer, or watchdog thread).

#### Verification: Phase 4

```
make clean && make verify
make test
```

**Behavioral test on target:**
1. Start venc standalone — SIGINT causes clean shutdown (same as before)
2. Start venc standalone — double SIGINT within 5 seconds force-exits
3. Start venc standalone — SIGHUP triggers config reload + reinit
4. Write a test that calls `venc_init()` + `venc_start()`, waits 3 seconds,
   calls `venc_stop()` from a different thread, verifies clean shutdown
   without signal handlers

---

### Phase 5: Optional HTTP API

**Goal:** Parent program can disable the built-in HTTP server and use the
`venc_set_*()` functions directly.

#### Step 5.1: Add httpd enable flag

In `venc_init()`:

```c
if (cfg->system.web_port != 0) {
    venc_httpd_start(ctx, cfg->system.web_port);
    venc_api_register(ctx, backend_name, &ctx->apply_cb);
}
```

When `web_port == 0`, no httpd thread is spawned and no API routes are
registered. The `venc_set_*()` functions work regardless.

#### Step 5.2: Implement public control functions

```c
int venc_set_bitrate(VencContext *ctx, uint32_t kbps) {
    pthread_mutex_lock(&ctx->cfg_mutex);
    ctx->cfg.video0.bitrate = kbps;
    int ret = ctx->apply_cb.apply_bitrate
        ? ctx->apply_cb.apply_bitrate(kbps) : -1;
    pthread_mutex_unlock(&ctx->cfg_mutex);
    return ret;
}

/* Same pattern for venc_set_fps, venc_set_gop, venc_request_idr */
```

These functions mirror what `handle_set()` does in venc_api.c but without
HTTP overhead.

#### Verification: Phase 5

```
make clean && make verify
make test
```

**Behavioral test on target:**
1. Set `web_port: 0` in config. Start venc. Verify no HTTP server is
   listening. Verify streaming works.
2. Set `web_port: 8080`. Start venc. Verify HTTP API works as before.
3. Write test harness: `venc_init()` with `web_port=0`, call
   `venc_set_bitrate(ctx, 5000)` at runtime, verify bitrate changes in
   the encoded stream.

---

### Phase 6: Build as Library

**Goal:** Produce `libvenc.a` for static linking into parent programs.

#### Step 6.1: Makefile changes

```makefile
# Object files for library (everything except main.o)
LIB_OBJS := $(filter-out $(BUILD_DIR)/main.o, $(ALL_OBJS))

# Static library target
$(BUILD_DIR)/libvenc.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

# Standalone binary links against the library
$(BUILD_DIR)/venc: $(BUILD_DIR)/main.o $(BUILD_DIR)/libvenc.a
	$(CC) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -lvenc $(LIBS)

# New top-level targets
libvenc: $(BUILD_DIR)/libvenc.a
install-lib: libvenc
	install -D $(BUILD_DIR)/libvenc.a $(DESTDIR)/usr/lib/libvenc.a
	install -D include/venc.h $(DESTDIR)/usr/include/venc.h
	install -D include/venc_config.h $(DESTDIR)/usr/include/venc_config.h
```

#### Step 6.2: Public header installation

Only two headers are public:
- `include/venc.h` — lifecycle + frame callback API
- `include/venc_config.h` — config structs (already SDK-free)

All other headers (`main.h`, `star6e.h`, `sigmastar_types.h`, `venc_api.h`,
`venc_httpd.h`, `sensor_select.h`, `backend.h`) remain internal and are not
installed.

#### Step 6.3: pkg-config file (optional)

```
# venc.pc
Name: libvenc
Description: SigmaStar video encoder library
Version: 1.0.0
Libs: -lvenc -lmi_vif -lmi_vpe -lmi_venc -lmi_isp -lmi_sensor -lmi_sys -lcus3a -lispalgo -lpthread
Cflags: -I${includedir}
```

#### Verification: Phase 6

```
make clean && make build         # standalone binary builds as before
make libvenc                     # produces libvenc.a
make verify                      # both backends build
make test                        # unit tests pass
```

**Integration test:** Write a minimal `waybeam_stub.c` that links against
`libvenc.a`:

```c
#include "venc.h"

int frame_count = 0;
void count_frames(const VencFrame *f, void *ud) {
    (void)f; (void)ud;
    frame_count++;
}

int main(void) {
    VencConfig cfg;
    venc_config_defaults(&cfg);
    cfg.system.web_port = 0;

    VencContext *ctx;
    if (venc_init(&ctx, &cfg) != 0) return 1;
    venc_set_frame_callback(ctx, count_frames, NULL);
    venc_start(ctx);

    sleep(5);
    venc_stop(ctx);
    venc_destroy(ctx);

    printf("Received %d frames in 5 seconds\n", frame_count);
    return 0;
}
```

Cross-compile and deploy:

```bash
$(CC) -o waybeam_stub waybeam_stub.c -I include/ -L build/ -lvenc $(SDK_LIBS)
```

Verify it runs on target, receives frames, and shuts down cleanly.

---

## WFB-NG Integration Compatibility

This modularization is designed to be fully compatible with the wfb-ng TX
integration plan owned by the `waybeam_wfb_ng` repository.

### Architecture: RTP Callback vs Direct Integration

The wfb-ng study proposes an **RTP passthrough** architecture where
`send_rtp_packet()` is the single integration point — complete RTP packets
(including H.265 FU fragmentation and VPS/SPS/PPS injection) are fed
directly into wfb-ng's FEC + encryption pipeline. The wfb-ng RX peer
delivers standard RTP to the ground station decoder.

The wfb-ng study's direct approach adds a `STREAM_MODE_WFB` enum and
`HAVE_WFB` compile flag inside venc's backend. With the modularized library,
a cleaner architecture uses the **RTP packet callback** (introduced in
Step 3.4) to achieve the same result without coupling:

```
┌─────────────────────────────────────────────────────┐
│  waybeam-hub (parent process)                       │
│                                                     │
│  ┌─────────┐   RTP callback     ┌──────────────┐   │
│  │ libvenc  │ ──────────────────►│ wfb_tx_shim  │   │
│  │          │  RTP hdr + payload │              │   │
│  │ FU frag  │  (iov-style)      │ wfb_tx_send  │   │
│  │ VPS/SPS  │                    │ _iov()       │   │
│  │ RTP hdr  │                    │ → FEC block  │   │
│  └─────────┘                    └──────┬───────┘   │
│                                        │            │
│                              ┌─────────▼─────────┐ │
│                              │ wfb-ng TX engine   │ │
│                              │ FEC + encrypt +    │ │
│                              │ raw socket inject  │ │
│                              └───────────────────┘ │
└─────────────────────────────────────────────────────┘
```

### Why RTP Callback, Not NAL Callback

The `VencFrameCallback` delivers raw NAL units from the SDK DMA buffer.
wfb-ng expects **complete RTP packets** — the output after FU fragmentation,
VPS/SPS/PPS prepend, and RTP header assembly. Using the NAL callback would
force waybeam-hub to duplicate all of venc's RTP encapsulation logic.

The `VencRtpCallback` fires at the exact point where `send_rtp_packet()`
currently calls `sendmsg()` — the same "single choke point" identified in
the wfb-ng study. The RTP stream is preserved end-to-end: venc builds RTP
packets, callback delivers them to wfb-ng, wfb-ng TX/RX passes them through
to the ground station decoder.

### Comparison: Direct vs Callback

| Aspect | Direct (STREAM_MODE_WFB) | RTP Callback (modularized) |
|--------|--------------------------|----------------------|
| Copies | 1 (into FEC block) | 1 (into FEC block via `send_iov`) |
| RTP preserved | Yes (same `send_rtp_packet()`) | Yes (callback at same point) |
| venc changes | Add wfb code to backend | None (callback exists) |
| Compile flags | `HAVE_WFB=1` needed | None |
| wfb-ng coupling | venc links libsodium + wfb objects | waybeam-hub links them |
| Backend duplication | Must wire into both star6e + maruko | One callback, backend-agnostic |
| Independent testing | Need wfb-ng built for each backend | Test venc and wfb-ng independently |
| FU fragmentation | Reused (inside venc) | Reused (inside venc) |

**Verdict:** The RTP callback approach is strictly better. Same copy count,
same RTP passthrough, no coupling, no compile flags, works with both
backends automatically.

### Live-Tunable WFB Parameters

The wfb-ng study proposes live-tunable radio and FEC parameters via HTTP API
(FEC k/n, MCS index, STBC, LDPC, guard interval, bandwidth, VHT mode). In
the direct integration model, these would be wired through venc's
`VencApplyCallbacks` and config struct.

With the modularized library, **wfb-ng parameters do not belong in venc at
all**. waybeam-hub manages them directly:

```c
/* waybeam-hub owns wfb-ng lifecycle and runtime tuning */
wfb_tx_set_fec(hub.tx, new_k, new_n);
wfb_tx_set_radio(hub.tx, &radio_cfg);
```

This means:
- **No `VencConfigOutgoing.wfb_*` fields** — venc config stays clean
- **No `HAVE_WFB` compile guards** in venc_api.c field table
- **No `apply_wfb_fec` / `apply_wfb_radio` callbacks** in VencApplyCallbacks
- **waybeam-hub exposes its own HTTP API** for wfb-ng parameters alongside
  venc parameters, presenting a unified control plane to the user

The wfb-ng study's `wfb://wlan0` URI scheme, mode-aware field visibility,
and `cmd_req_t`/`cmd_resp_t` internalization all become waybeam-hub concerns
rather than venc concerns.

### VencContext: No WFB State

Because wfb-ng is managed by waybeam-hub (not venc), `VencContext` does
**not** include any wfb-ng state. No `wfb_tx_t *`, no radio parameters, no
FEC config. The context struct stays focused on video encoding:

```c
struct VencContext {
    /* ... existing fields ... */

    /* RTP packet callback (for wfb-ng integration via waybeam-hub) */
    void *rtp_cb;
    void *rtp_cb_user;

    /* Frame callback (for recording, analysis) */
    void *frame_cb;
    void *frame_cb_user;

    /* No wfb-ng state here — managed by parent program */
};
```

### waybeam-hub as Orchestrator

With both venc and wfb-ng modularized, waybeam-hub becomes the orchestrator
that connects them via the RTP callback and manages both lifecycles:

```c
/* waybeam-hub main */
#include "venc.h"
#include "wfb_tx_shim.h"

typedef struct {
    VencContext *vctx;
    wfb_tx_t   *tx;
} HubContext;

static void on_rtp_packet(const VencRtpPacket *pkt, void *ud) {
    HubContext *hub = ud;
    /* RTP passthrough: feed assembled RTP packet into FEC + encrypt.
     * Uses iov variant to avoid flattening — 1 copy into FEC block. */
    wfb_tx_send_iov(hub->tx, pkt->header, 12,
                    pkt->payload, pkt->payload_len, 0);
}

int main(void) {
    HubContext hub = {0};

    /* Init wfb-ng TX */
    wfb_tx_config_t wfb_cfg = {
        .keypair_path = "/etc/drone.key",
        .fec_k = 8, .fec_n = 12,
        .wlan = "wlan0",
    };
    hub.tx = wfb_tx_create(&wfb_cfg);

    /* Init venc */
    VencConfig vcfg;
    venc_config_defaults(&vcfg);
    venc_config_load("/etc/venc.json", &vcfg);
    vcfg.system.web_port = 0;      /* hub has its own control plane */
    vcfg.outgoing.server[0] = '\0'; /* no built-in UDP send */

    VencContext *vctx;
    venc_init(&vctx, &vcfg);
    hub.vctx = vctx;

    /* RTP callback feeds wfb-ng directly */
    venc_set_rtp_callback(vctx, on_rtp_packet, &hub);
    venc_start(vctx);

    /* Hub event loop: expose unified HTTP API for both venc and wfb-ng.
     * venc params: venc_set_bitrate(hub.vctx, ...) etc.
     * wfb params:  wfb_tx_set_fec(hub.tx, ...) etc. */

    venc_stop(vctx);
    venc_destroy(vctx);
    wfb_tx_destroy(hub.tx);
}
```

### Standalone venc with WFB (Without waybeam-hub)

For users who want wfb-ng without waybeam-hub, venc can still support
`STREAM_MODE_WFB` as a **standalone-only** feature — wired in `main.c`
rather than the library:

```c
/* main.c — standalone wfb mode */
if (stream_mode == STREAM_MODE_WFB) {
    wfb_tx_t *tx = wfb_tx_create(&wfb_cfg);
    venc_set_rtp_callback(ctx, standalone_wfb_on_rtp, tx);
    /* ... */
}
```

This keeps the library clean while supporting standalone deployment.
The `HAVE_WFB` compile flag affects only `main.c` and the link step,
not the library itself.

### Dual Output Support

The RTP callback + built-in UDP sender are not mutually exclusive. If
`outgoing.server` is set, venc continues sending via UDP/RTP AND delivers
RTP packets via callback. This enables:

- **RTP Callback → wfb-ng** (FPV downlink via radio)
- **UDP → local recorder** (simultaneous recording on ground station)

Or with `outgoing.server` empty, callback-only mode for minimal overhead.

### Audio over WFB-NG

The `VencAudioCallback` (described in the Audio section below) follows the
same pattern. waybeam-hub can route audio frames through a separate wfb-ng
channel:

```c
static void on_audio(const VencAudioFrame *frame, void *ud) {
    HubContext *hub = ud;
    /* Audio on separate wfb-ng channel or same channel with demux */
    wfb_tx_send(hub->audio_tx, frame->data, frame->len, 0);
}

venc_set_audio_callback(ctx, on_audio, &hub);
```

Audio does not need RTP-level callbacks because audio frames are small
(typically under MTU) and do not require FU fragmentation.

---

## Audio Integration Compatibility

This modularization is designed to accommodate the planned audio capture
feature (see `AUDIO_UDP_OUTPUT_FEASIBILITY.md`). Audio adds a separate
capture thread using MI_AI (the SigmaStar audio input SDK), an independent
UDP socket, and new `VencConfigAudio` config fields.

### Impact on VencContext

Audio state naturally fits into the context struct. The `AudioState` globals
proposed in the audio study (`g_audio.*`) become context members:

```c
struct VencContext {
    /* ... existing fields from Phase 1 ... */

    /* Audio capture (added by audio feature, not modularization) */
    struct {
        bool            loaded;
        bool            running;
        pthread_t       thread;
        int             socket_handle;
        struct sockaddr_in dst;
        volatile sig_atomic_t stop;
    } audio;
};
```

This follows the same pattern as the video pipeline state — per-context,
no globals, clean lifecycle.

### Audio Frame Callback

The `VencFrameCallback` is video-specific (NAL units, keyframe detection).
Audio needs its own callback type:

```c
typedef struct {
    const uint8_t *data;       /* audio samples or encoded frame */
    size_t         len;
    uint8_t        codec_id;   /* 0=PCM, 1=G.711A, 2=G.711U, 3=G.726 */
    uint32_t       sample_rate;
    uint32_t       channels;
} VencAudioFrame;

typedef void (*VencAudioCallback)(const VencAudioFrame *frame, void *user_data);

int venc_set_audio_callback(VencContext *ctx, VencAudioCallback cb,
                            void *user_data);
```

This follows the same design as the video callback — called from the audio
thread, data valid only during callback, zero-copy from MI_AI buffer.

### waybeam-hub with Audio + Video + WFB-NG

```c
static void on_video(const VencFrame *frame, void *ud) {
    HubContext *hub = ud;
    wfb_tx_send(hub->tx, frame->data, frame->len,
        frame->is_keyframe ? WFB_PKT_FLAG_KEY : 0);
}

static void on_audio(const VencAudioFrame *frame, void *ud) {
    HubContext *hub = ud;
    /* Multiplex audio over same wfb-ng channel, or separate channel */
    wfb_tx_send(hub->audio_tx, frame->data, frame->len, 0);
}

/* In init: */
venc_set_frame_callback(ctx, on_video, &hub);
venc_set_audio_callback(ctx, on_audio, &hub);
```

### Implementation Order Independence

Audio and modularization are independent work streams:

- **Audio first:** Audio can be implemented with the current global-state
  architecture (as the audio study proposes). The `g_audio` globals work
  fine for single-instance.
- **Modularization first:** The context struct is designed with audio in
  mind. When audio is later implemented, its state slots into `VencContext`
  instead of globals.
- **Either order works.** If audio lands first, Phase 1 of modularization
  moves `g_audio` into the context struct alongside the video globals. If
  modularization lands first, audio is implemented directly against the
  context struct.

### No Phase Changes Required

Audio does not add or modify any modularization phase. The audio thread,
socket, and MI_AI lifecycle are orthogonal to the video pipeline refactoring.
The only touchpoint is ensuring `VencContext` has space for audio state
(trivial struct extension) and that `venc_destroy()` calls `audio_teardown()`.

---

## Summary: Phase Dependencies and Gate Criteria

```
Phase 1 ──► Phase 2 ──► Phase 3 ──► Phase 4 ──► Phase 5 ──► Phase 6
Context     Split       Frame       Signal      Optional    Build as
struct      entrypoint  callback    decouple    httpd       library
```

Each phase is independently deployable. The standalone binary works at
every stage. Phases can be merged to main individually.

| Phase | Gate Criteria |
|-------|---------------|
| 1 | `make verify` passes. HTTP API works on target. Unit tests pass. |
| 2 | `make verify` passes. Reinit (SIGHUP + API) works on target. |
| 3 | `make verify` passes. Frame callback delivers frames on target. Standalone streaming unchanged. |
| 4 | `make verify` passes. SIGINT/SIGHUP work in standalone. Library usable without signals. |
| 5 | `make verify` passes. `web_port=0` disables httpd. `venc_set_*()` control works. |
| 6 | `make libvenc` produces archive. Stub program links and runs on target. |
