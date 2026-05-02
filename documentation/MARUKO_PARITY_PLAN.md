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
| ~~Aspect-ratio precrop~~ | ~~`star6e_pipeline.c:338-386`~~ | **Closed in v0.9.8** — `configure_maruko_scl()` writes a centered rect via `pipeline_common_compute_precrop()` (Star6E parity); `venc_config.h:51-54` comment updated. | — |
| ~~Debug OSD overlay~~ | ~~`star6e_runtime.c:825-835`, `star6e_pipeline.c:1106-1108`~~ | **Code wired in v0.9.9** (default-off); Maruko runtime path blocked by SDK kernel/lib vintage mismatch — see Phase 2b. | — |
| IMU / BMI270 | `star6e_pipeline.c:1084-1102` + `star6e_runtime.c:827,848,857` | No `imu_bmi270` references in maruko sources | Easy if board has BMI270 |
| Live AR-change reinit | Star6E SIGHUP rebuilds VIF/VPE for AR change | `maruko_runtime.c:99-107` forces sensor mode lock to avoid ISP hang | Medium-arch |
| `apply_mute` | `star6e_controls.c:1139` | NULL (`maruko_controls.c:1051`) — gated by audio absence | Trivial after audio lands |
| Audio capture (MI_AI) | `star6e_audio.c` 738 lines | Inert; runtime emits "audio output is not supported" warning (`maruko_runtime.c:58-60`) | High |
| SD card recording (HEVC + TS mux) | `star6e_recorder.c` 294 + `star6e_ts_recorder.c` 421 | No record callbacks; no `record.*` hooks in maruko runtime | High |
| Dual VENC (Gemini mode) | `star6e_pipeline.c:1335-1426`, `star6e_runtime.c:528-555` | None | High — needs SDK probe |

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
  `venc_config.h:51-54` (done with Phase 1 in v0.9.8).

### Phase 1 — Aspect-ratio SCL precrop (DONE, v0.9.8)

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

### Phase 2 — Debug OSD overlay (CODE LANDED v0.9.9; runtime blocked)

**Status:** code parity with Star6E is in place and Maruko `make verify`
passes.  The opt-in flag `debug.showOsd=true` is honored end-to-end:
`debug_osd_create()` after VENC start in
`maruko_pipeline_configure_graph()`, per-frame
`begin/sample_cpu/text/end_frame` in `maruko_pipeline_process_stream()`,
`debug_osd_destroy()` at the top of `maruko_pipeline_teardown_graph()`.
`debug_osd.c` + `debug_osd_draw.c` are now in `HELPER_SRC`.  Default-off
behaviour leaves both backends untouched.

**Runtime blocker (192.168.2.12 / SSC378QE OpenIPC):** invoking
`MI_RGN_Init` triggers a kernel Oops in `MI_DEVICE_Ioctl` (kfree path)
and wedges the venc encode loop — the **same** lib/kernel vintage
mismatch documented in `memory/maruko_osd_render_bringup.md`.  Until
Phase 2b ships the cure, the runtime path on Maruko is
**safety-gated**: `debug.showOsd=true` logs a one-time warning at
`maruko_pipeline_configure_graph()` and skips the attach so a stale
config never hangs venc.  Replace the gate with the actual
`debug_osd_create()` call once Phase 2b lands.

### Phase 2b — Maruko OSD runtime fix (1-2 days, blocked on dep ordering work)

The waybeam-hub team got Maruko OSD render working on the same hardware.
Lift their recipe into venc:

- [ ] **Pre-load deps in order** with `RTLD_NOW | RTLD_GLOBAL`:
  `libcam_os_wrapper.so` → `libmi_common.so` → `libmi_sys.so` →
  `libmi_rgn.so`.  None declare NEEDED entries, so lazy resolution loses
  exported stubs from the executable.
- [ ] **Module ID for OSD attach on Maruko is `MI_MODULE_ID_SCL` = 34**,
  NOT `E_MI_RGN_MODID_VPE = 0` that `debug_osd.c:231` currently
  hardcodes.  Add a build-time `#ifdef PLATFORM_MARUKO` (or pass through
  `vpe_port` parameter properly) so the module ID matches the SCL
  channel that Maruko binds.
- [ ] **OpenIPC struct layout drift.**  `MI_RGN_OsdChnPortParam_t` may
  include a trailing `stColorInvertAttr` field on Maruko.  Cross-check
  against the OpenIPC msposd-vintage headers (vendored in
  `waybeam-hub/vendor/sigmastar/maruko/include/`).
- [ ] **OSD init must run before any worker thread is spawned in the
  process.**  `MI_VENC_StartRecvPic` creates `[venc0_P0_MAIN]`
  internally, so on Maruko `debug_osd_create()` must move ahead of
  `maruko_start_venc()`'s recv-pic call (or VENC start).  Reorder
  carefully and verify Star6E still works.
- [ ] **Re-verify:** `showOsd=true` boots clean, OSD shows fps/cpu in
  the stream, no `dmesg` Oops.  Soak 30s+ with concurrent IQ and
  recording load.

### Phase 3 — IMU / BMI270 wiring (1 day, gated on hardware)

**Why now:** small code change, but only if `192.168.2.12` actually has a
BMI270 wired.

- **Step 0 (probe):** SSH to `192.168.2.12`, check `/sys/bus/i2c/devices/`
  and `/dev/i2c-*` for BMI270 presence. Skip phase if absent.
- If present: copy `imu_init` callsite into `maruko_pipeline.c` after VENC,
  plus `imu_bmi270_push` into the run loop. Pattern:
  `star6e_pipeline.c:1084-1102` + `star6e_runtime.c:827`.
- **Verify:** sidecar telemetry includes IMU samples.

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

### Phase 7 — Dual VENC probe + Gemini mode (≈2 days probe + ≈2 days port if green)

**SDK probe first, port second.**

- [ ] Probe: stand up a minimal test that calls
  `MI_VENC_CreateChn(1, ...)` after channel 0 is running on Maruko. If
  SDK rejects (returns -1) we stop.
- [ ] If green: port `star6e_pipeline_start_dual()`
  (`star6e_pipeline.c:1335-1426`) + adaptive bitrate throttler
  (`star6e_runtime.c:322-420`).
- [ ] Re-evaluate: if dual VENC works, revisit Phase 6 to enable
  `record.mode="dual"`/`"dual-stream"`.

### Phase 8 — Maruko sensor depth (deferred, driver-gated)

Mode/fps mapping, direct ISP-bin load stability, >30fps verification. The
`CURRENT_STATUS` says "deferred until newer driver." Keep deferred, but
probe at the start of each phase to see if a newer driver landed.

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
