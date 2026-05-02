# Maruko ↔ Star6E Feature Parity Plan

Living plan for bringing the Maruko backend up to feature parity with Star6E.
Re-evaluate after every phase. Update this file when re-ordering or
dropping phases.

## Code-verified current state (May 2026)

Audit was driven by reading source, not summaries. Differs from
`CURRENT_STATUS_AND_NEXT_STEPS.md` and `DUAL_BACKEND_SPLIT_PLAN.md` in
several places (see "Surprises vs docs" below).

### Already at parity

| Area | Evidence |
|---|---|
| HTTP API callbacks | `star6e_controls.c:1130-1149` vs `maruko_controls.c:1042-1061` — identical except `apply_mute=NULL` on Maruko |
| Codec / rate control | `star6e_pipeline.c:521-552`, `maruko_pipeline.c:638-776` — H.264/H.265 + CBR/VBR/AVBR both sides |
| Sensor unlock (IMX415/IMX335 cold-boot latch) | `maruko_pipeline.c:930-933` |
| Scene detector + IDR insert | `maruko_runtime.c:27-28`, sidecar telemetry wired (`maruko_pipeline.c:1295,1489`) |
| RTP sidecar / transport telemetry | `query_transport_status` bound on both |
| IQ system (AE/AWB tuning) | `maruko_iq.c` (736 lines) ≈ `star6e_iq.c` (800 lines) parity |
| Frame-lost overshoot protection | shared `pipeline_common_frame_lost_threshold()` |

### Verified gaps on Maruko

| Gap | Star6E location | Maruko status | Effort |
|---|---|---|---|
| ~~Aspect-ratio precrop~~ | ~~`star6e_pipeline.c:338-386`~~ | **Closed in v0.9.9** — `configure_maruko_scl()` writes a centered rect via `pipeline_common_compute_precrop()` (Star6E parity); `venc_config.h:51-54` comment updated. | — |
| ~~Debug OSD overlay~~ | ~~`star6e_runtime.c:825-835`, `star6e_pipeline.c:1106-1108`~~ | **Closed in v0.9.10 (code) + v0.9.11 (runtime)** — full Star6E parity, runtime verified live on 192.168.2.12. | — |
| ~~IMU / BMI270~~ | ~~`star6e_pipeline.c:1084-1102` + `star6e_runtime.c:827,848,857`~~ | **Closed in v0.9.13 (PR #84)** — BMI270 detected at 0x68 / `i2c-1` after sensor swap, port verified live: 1963 samples drained over 9 s @ 118 fps, 0 errors. Maruko-specific ordering caveat: `imu_init` must precede `bind_maruko_pipeline` / `MI_VENC_StartRecvPic` (Star6E pattern stalls the encoder fd's `poll()`). | — |
| Live AR-change reinit | Star6E SIGHUP rebuilds VIF/VPE for AR change | `maruko_runtime.c:99-107` forces sensor mode lock to avoid ISP hang | Medium-arch |
| `apply_mute` | `star6e_controls.c:1139` | NULL (`maruko_controls.c:1051`) — gated by audio absence | Trivial after audio lands |
| Audio capture (MI_AI) | `star6e_audio.c` 738 lines | Inert; runtime emits "audio output is not supported" warning (`maruko_runtime.c:58-60`) | High |
| SD card recording (HEVC + TS mux) | `star6e_recorder.c` 294 + `star6e_ts_recorder.c` 421 | No record callbacks; no `record.*` hooks in maruko runtime | High |
| Dual VENC (Gemini mode) | `star6e_pipeline.c:1335-1426`, `star6e_runtime.c:528-555` | None | High — needs SDK probe |
| 3A perf throttle | n/a (Star6E has cus3a; Maruko's NATIVE 3A_Proc_0 spends ~60% CPU at 120 fps) | **Closed in v0.9.12 (PR #83)** — opt-in `isp.aeMode="throttle"` swaps SDK NATIVE AE for a no-op AE adaptor + 15 Hz manual `SetAeParam`; saves ~24% sys CPU at 120 fps. Default `"native"` preserves existing behaviour. | — |

### N/A on Maruko (SDK-limited)

- **Custom 3A daemon** (`star6e_cus3a.c`, 562 lines): no `MI_ISP_CUS3A`
  surface in current Maruko SDK. Status: blocked by SDK, not by us.
  Revisit when Maruko SDK refresh lands.

### Surprises vs existing docs

1. **Rate-control overshoot floor** — memory said "120% threshold + 512kbps
   floor for Star6E only", but it is shared via
   `pipeline_common_frame_lost_threshold()` and runs on Maruko too.
2. **Sensor unlock cold-boot latch** is on Maruko
   (`maruko_pipeline.c:930-933`); `CURRENT_STATUS` implied Star6E-only.
3. **Maruko reinit locks the sensor mode** (`maruko_runtime.c:99-107`) —
   explicit constraint not called out in `DUAL_BACKEND_SPLIT_PLAN`.

---

## Phased plan (Quick wins → architectural)

Each phase ends with build-both + remote test on `192.168.2.12` (Maruko
bench, imx415) before moving on. Re-evaluate after every phase.

### Phase 0 — Bookkeeping (≈30 min)

Update stale docs so the plan starts from a true baseline.

- [ ] Rewrite `DUAL_BACKEND_SPLIT_PLAN.md` "Maruko Follow-Up Backlog" against
  verified gaps above.
- [ ] Mark cold-boot unlock + frame-lost protection as "ported" in
  `CURRENT_STATUS_AND_NEXT_STEPS.md`.
- [x] Replace the "until SCL crop port lands" comment in
  `venc_config.h:51-54` (done with Phase 1 in v0.9.9).

### Phase 1 — Aspect-ratio SCL precrop (DONE, v0.9.9)

**Why first:** documented gap, narrow blast radius, no SDK probing needed,
immediately fixes geometry on every non-16:9 encode.

- [x] Compute center-crop rect when encode AR ≠ sensor mode AR
  (re-using `pipeline_common_compute_precrop()`; computed against the
  post-binning effective input so it matches what actually reaches the
  SCL stage).
- [x] Wire into `configure_maruko_scl()`'s `scl_port.crop`.
- [x] Default `isp.keepAspect=true`; `false` falls back to zero-crop
  (full sensor → downstream stretch).
- [x] Hooked into `venc_api_set_active_precrop()` /
  `clear_active_precrop()` for `/api/v1/config` parity with Star6E.
- [x] **Verified on `192.168.2.12` (IMX415):**
  - 960x720 (4:3) on 1920x1080 sensor mode → `Precrop: 1920x1080 ->
    1440x1080 (offset 240,0)`, encoding 89 fps @ 25 Mbps cleanly,
    visually correct on screen.
  - 1280x720 (16:9) on 1920x1080 → no precrop, full source.
  - 4:3 with `keepAspect=false` → no precrop (legacy stretch path).

### Phase 2 — Debug OSD overlay code parity (DONE, v0.9.10)

Code parity with Star6E in place and the opt-in flag `debug.showOsd=true`
honored end-to-end on both backends:
`debug_osd_create()` after VENC start in
`maruko_pipeline_configure_graph()`, per-frame
`begin/sample_cpu/text/end_frame` in `maruko_pipeline_process_stream()`,
`debug_osd_destroy()` at the top of `maruko_pipeline_teardown_graph()`.
`debug_osd.c` + `debug_osd_draw.c` are in `HELPER_SRC`.  Default-off
leaves both backends untouched.  v0.9.10 shipped a temporary safety gate
(WARN-and-skip) on Maruko while Phase 2b investigated the runtime
crash; that gate was removed in Phase 2b.

### Phase 2b — Maruko OSD runtime fix (DONE, v0.9.11)

Root cause turned out NOT to be the lib/kernel vintage mismatch
originally suspected — it was a build-time conditional bug in
`src/debug_osd.c`.  The Maruko build defines BOTH `-DPLATFORM_STAR6E`
and `-DPLATFORM_MARUKO` (Star6E shim headers reused for type compat;
see `Makefile:39`).  `debug_osd.c` started with `#ifdef PLATFORM_STAR6E`,
so the Star6E ABI branch (1-arg `MI_RGN_Init`, mod_id 0 = VPE, 3-arg
`AttachToChn`) was compiled into the Maruko binary too — and the Star6E
ABI ran against the Maruko kernel/lib pair, producing the
`MI_DEVICE_Ioctl → kfree → compound_head` oops with a userspace-shaped
pointer reaching kfree.  Cure was a one-line conditional fix plus the
expected dep-preload + module-ID-34 + init-before-kthread changes:

- [x] **Build-time conditional fix.**  `src/debug_osd.c` first
  conditional now reads `#if defined(PLATFORM_STAR6E) && !defined(PLATFORM_MARUKO)`,
  so Maruko binaries enter the Maruko ABI branch as intended.
- [x] **Maruko ABI branch active.**  Targets the OpenIPC libmi_rgn.so
  v3 API (`MI_RGN_Init(soc_id, palette*)`, 3-arg `MI_RGN_Create`, 4-arg
  `MI_RGN_AttachToChn`, 64-bit `MI_PHY` / pointer-width `MI_VIRT` in
  `CanvasInfo_t`, module ID 34 = `E_MI_MODULE_ID_SCL`).
- [x] **Pre-load `libmi_rgn.so`.**  Added to the existing
  RTLD_GLOBAL dep chain in `maruko_mi_init()` alongside
  `libcam_os_wrapper`, `libmi_common`, `libispalgo`, `libcus3a`.
- [x] **Init-before-kthread ordering.**  `debug_osd_create()` now
  runs ahead of `bind_maruko_pipeline()` and any
  `MI_VENC_StartRecvPic` so the v5.10 OpenIPC `mi_rgn` driver's
  singlethread workqueue is created from the main task.
- [x] **Phase 2 safety gate dropped.**
- [x] **Verified on 192.168.2.12** (OpenIPC SSC378QE / IMX415
  1472x816@120, kernel 5.10.61): RGN init/create/attach/getcanvas
  all succeed, encode loop runs at ~117 fps, no kernel taint, OSD
  canvas mapped at 1472x816 stride 736.

Recipe cross-referenced with `waybeam-hub/src/rgn_backend_maruko.c`,
which had already verified the same pattern against the same kernel/lib.

### Phase 3 — IMU / BMI270 wiring (DONE, v0.9.13 — Maruko ordering caveat)

**Re-probe outcome (2026-05-02, after sensor module swap on
192.168.2.12):** BMI270 detected at `0x68` on `/dev/i2c-1` (chip ID
`0x24`). Earlier "no IMU on this board" result was a missing/loose
sensor pack, not a hardware-class limitation.

**What shipped (PR #84, branch `feature/maruko-imu-bmi270`):**

- [x] `src/imu_bmi270.c` moved from `STAR6E_ONLY_SRC` to `HELPER_SRC`
  in the Makefile so both backends pull it in.
- [x] `MarukoBackendConfig.imu` mirrors `VencConfig.imu`; defaulted in
  `maruko_config_defaults`, copied through in `maruko_config_from_venc`.
- [x] `MarukoBackendContext.imu` lifecycle wired in
  `maruko_pipeline.{configure_graph,process_stream,teardown_graph}`:
  `imu_init` + `imu_start` at graph configure time, `imu_drain` per
  iteration before `MI_VENC_GetStream`, `imu_stop`/`destroy` in teardown.
- [x] Push callback is a no-op stub (`maruko_pipeline_imu_push`) for
  this phase — sample sink is reserved for follow-up consumers
  (sidecar, recording, etc.).

**Maruko-specific ordering caveat (worth carrying forward to any
new "thread starts a long bring-up before VENC starts" feature):**

- On Star6E, IMU init is fine *after* `bind_maruko_pipeline` /
  `MI_VENC_StartRecvPic`.
- On Maruko (this hardware, OpenIPC libmi_venc), running the
  ~2 s BMI270 auto-bias loop **after** `MI_VENC_StartRecvPic` leaves the
  VENC fd in a state where `poll(POLLIN)` never wakes — the stream loop
  prints "waiting for encoder data..." indefinitely (observed 12 s +).
- Cure: call `imu_init` *before* `bind_maruko_pipeline` in
  `maruko_pipeline_configure_graph`. With `calSamples=400` the encoder
  recovers cleanly: live test showed 1963 IMU samples drained over 9 s
  at 118 fps with 0 errors and clean teardown.
- A short calibration window (`calSamples=50`, ~250 ms) also worked,
  but the ordering fix is the proper cure rather than rationing the
  sample count.
- Inline comment in `maruko_pipeline.c:configure_graph` documents the
  constraint so a future cleanup that "unifies" the lifecycle order
  doesn't silently re-introduce the stall.

**Verified on 192.168.2.12** (OpenIPC SSC378QE / IMX415, BMI270):
`json_cli -s 'imu.enabled=true'` followed by `venc -c /etc/venc.json`,
9 s capture, 1963 IMU samples, 0 errors, encoder steady at 118 fps,
no warnings during teardown.

### Phase 4 — Sensor-mode unlock on reinit (medium-architectural, plan ahead)

**Why here:** unblocks live `video0.size` AR-change for Maruko, which is a
prerequisite for any future feature that wants per-channel resolution
(recording, dual-stream). Doing this before audio/recording avoids painting
into a corner.

- **Step 0 (investigation):** reproduce the "ISP hang on mode switch"
  mentioned in `maruko_runtime.c:99-107`. Capture dmesg + venc.log.
- Try the same `VPE_SCL_preset_shutdown` sysfs clock trick that fixed
  Star6E (`star6e_pipeline.c:60-77`) — write to `/sys/.../clk_vpe`,
  `clk_scl` after `MI_SYS_Exit` to reset kernel `already_inited` flag.
- If that fails, gate the unlock behind a config flag
  (`isp.allow_mode_switch=false` default) so we ship a safe-mode fallback.
- **Re-evaluation gate:** if reinit unlock proves expensive (>3 days),
  skip it and proceed to Phase 5 — Maruko users keep restart-required mode
  switching. Document the decision in `HISTORY.md`.

### Phase 5 — MI_AI shim + audio capture port (≈3-4 days)

**Architectural prep:** Maruko's MI shim layer (`maruko_mi.c`) currently
dlopens `MI_VENC`, `MI_VIF`, `MI_VPE`, `MI_ISP`, `MI_SCL`, `MI_SYS`.
Audio adds `MI_AI` + `MI_AO`.

- [ ] Extend `maruko_mi.{c,h}` with `MI_AI` symbol table (mirror
  `star6e_mi.c`).
- [ ] Vendor `libmi_ai.so` / `libmi_ao.so` (i6c) under `libs/maruko/`.
- [ ] Port `star6e_audio.c` ↔ `maruko_audio.c`. Most of the file is
  SDK-agnostic ring buffer + Opus / G.711 encoding; only
  `MI_AI_GetFrame`/`SetFrame` calls change shape.
- [ ] Bind `apply_mute` callback in `maruko_controls.c:1051`.
- [ ] Add audio_ring → output_socket plumbing.
- **Verify:** Opus + PCM both reach `192.168.2.2` audio_port on bench.

### Phase 6 — SD card recording (≈2-3 days, depends on Phase 4-or-not)

**Architectural choice:** record-only single-VENC first; concurrent
stream+record only after Phase 7 dual-VENC probe.

- TS mux engine (`src/ts_mux.c`) is already platform-agnostic. Reuse it
  directly.
- Add `maruko_recorder.c` that mirrors `star6e_recorder.c` lifecycle but
  binds to the single existing VENC channel (`record.mode="mirror"` only,
  initially).
- Wire `record_status_callback` and `/api/v1/record/*` HTTP endpoints
  (already shared via `venc_api.c`).
- **Verify:** TS file rotates by time + size on bench.

### Phase 7 — Dual VENC probe + Gemini mode (PROBE PASSED 2026-05-02; port pending)

**SDK probe first, port second.**

- [x] **Probe passed on 192.168.2.12** (OpenIPC SSC378QE / IMX415,
  branch `feature/maruko-dual-venc-probe`, env-gated via
  `MARUKO_DUAL_VENC_PROBE=1`).  All three SDK stages returned 0 after
  channel 0 was running:
  - `MI_VENC_CreateChn(dev=0, chn=1, &attr)` → `ret=0`
  - `MI_VENC_SetInputSourceConfig(chn=1, RING_DMA)` → `ret=0`
  - `MI_VENC_StartRecvPic(chn=1)` → `ret=0`
  Channel 0 continued streaming at 118 fps after channel 1 was torn
  down — probe is non-disruptive.  Probe code is small (~50 lines,
  env-gated, no production impact) and lives in
  `maruko_start_venc()` so a future SDK refresh can be re-checked
  with one env var.
- [ ] Port `star6e_pipeline_start_dual()`
  (`star6e_pipeline.c:1335-1426`) + adaptive bitrate throttler
  (`star6e_runtime.c:322-420`).  Open question for the port: the
  Maruko VPE → VENC binding goes via `maruko_mi_sys_bind_chn_port2`
  (different ABI from Star6E `MI_SYS_BindChnPort2`); needs the same
  `src_fps` / `dst_fps` semantics as Star6E so per-channel FPS skip
  works.
- [ ] Re-evaluate: with dual VENC viable, revisit Phase 6 to enable
  `record.mode="dual"` / `"dual-stream"`.

### Phase 8 — Maruko sensor depth (deferred, driver-gated)

Mode/fps mapping, direct ISP-bin load stability, >30fps verification. The
`CURRENT_STATUS` says "deferred until newer driver." Keep deferred, but
probe at the start of each phase to see if a newer driver landed.

### Phase 9 — CPU perf: opt-in 3A throttle (DONE, v0.9.12 — PR #83)

Shipped as PR #83 (`feature/maruko-cus3a-throttle`, stacked on PR #81).
Replaces the SDK `3A_Proc_0` thread (NATIVE algorithm running at sensor
frame rate) with a no-op AE adaptor registered via
`CUS3A_RegInterfaceEX(ADAPTOR_1)` and a 15 Hz supervisory thread that
drives AE manually via `MI_ISP_CUS3A_SetAeParam`. AWB stays on the SDK
NATIVE path. Saves ~24% sys CPU at 120 fps on Cortex-A7 (60% → 36% sys);
IQ knobs still respond instantly because `MI_ISP_EnableUserspace3A`
keeps the IQ→HW pump alive.

- Opt-in via config field `isp.aeMode = "throttle"`; default
  `"native"` preserves existing behaviour and gives a safety hatch if a
  different sensor / firmware breaks the no-op adaptor.
- Verified on 192.168.2.12: `native` ~50% CPU, `throttle` ~36% CPU at
  120 fps.

---

## Re-evaluation triggers

Revisit and rewrite this plan if any of these happen:

1. Phase 4 sensor-mode unlock investigation costs >3 days → drop unlock,
   ship config-gated lock + restart-required path. Phase 6 falls back to
   "record at startup-fixed resolution only".
2. Phase 7 dual-VENC SDK probe fails → drop Gemini mode entirely on
   Maruko. Update `HISTORY.md` and `documentation/SD_CARD_RECORDING.md` to
   mark Maruko as "single-channel record only".
3. New Maruko driver lands during the work → reorder Phase 8 sensor-depth
   ahead of audio if it unblocks higher-FPS streaming (more user-visible
   than audio).
4. A BMI270-equipped Maruko board appears → reopen Phase 3. **(Done —
   board appeared 2026-05-02, Phase 3 closed in v0.9.13.)**

## Open work / next decision point

Phases 1, 2, 2b, 3, 7-probe, and 9 are closed. Live branches/PRs:
#81 (Phases 1/2/2b), #83 (Phase 9), #84 (Phase 3),
`feature/maruko-dual-venc-probe` (Phase 7 probe, PR pending).
Next agenda in priority order:

1. **Phase 7 dual-VENC port** — probe passed; full port now.  Mirrors
   Star6E `start_dual()` + bind via Maruko VPE port + adaptive
   bitrate throttler + `record.mode="dual"` config plumbing.  Cost:
   ~2 days.  Needs to land alongside or before Phase 6 because the
   recording mode selector picks between "mirror chn 0" and
   "dedicated record chn 1".
2. **Phase 5 (audio)** — biggest standalone gap (Opus/G.711 + MI_AI
   shim).  Independent of Phase 4/6/7, so can run in parallel with
   Phase 7 port if hardware bandwidth allows.
3. **Phase 4 (live AR-change reinit)** — medium-arch, unblocks
   per-channel resolution.  Prerequisite for clean Phase 6 recording
   only if record/stream want different resolutions; with dual VENC
   working that pressure is reduced.  Step 0 (reproduce ISP hang +
   capture dmesg + venc.log) is the cheap part.

Recommendation: pick Phase 5 (audio) next.  Phase 7 port is now de-
risked but still ~2 days of careful binding work; audio is bigger
but independent and more user-visible.  Order is genuinely a user
priority call now that the probe outcome is known.

## Architectural notes worth carrying

- **MI shim layer** is the dlopen abstraction (`maruko_mi.{c,h}`). Every
  new SDK API needs an entry there. Phase 5 (audio) is the first time we
  extend it; once the pattern is solid, future SDK ports are templated.
- **Reinit pattern divergence:** Star6E uses respawn-after-exit, Maruko
  uses in-process loop. Don't unify yet — let Phase 4 settle first, then
  revisit at end of Phase 7.
- **Config field gating:** prefer `isp.allow_mode_switch=false` and
  `record.mode="mirror"` defaults on Maruko until each phase ships. This
  avoids surprise behavior changes for existing users.
