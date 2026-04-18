# History

## [0.7.8] - 2026-04-18

- **WebUI reinit + IDR fixes.** Four related bugs in the reinit/save
  path and one missing IDR-on-bitrate behaviour.
- **Fix #1 — FPS kick on live reinit.** `star6e_pipeline_reinit`
  (`src/star6e_pipeline.c`) now re-kicks `MI_SNR_SetFps` at the end so
  a live FPS change actually reconfigures sensor timing.  Previously
  the kick only fired during the initial `star6e_pipeline_start_vpe`
  legacyAe branch and the once-per-process CUS3A `fps_kick_done`
  gate — neither re-armed on reinit, so the sensor stayed stuck at
  its cold-boot timing (e.g. 100 fps when 120 was requested).
- **Fix #2 — Save & Restart actually saves.** Added
  `venc_api_set_config_path()` (called by star6e_runtime and
  maruko_runtime with `VENC_CONFIG_DEFAULT_PATH`).  `handle_restart`
  and `process_restart_set_query` now call `venc_config_save()` before
  triggering reinit, so `MUT_RESTART` field changes and the explicit
  "Save & Restart" button both persist to `/etc/venc.json` before the
  reload-from-disk round-trip.
- **Fix #3 — Restore Defaults actually restores.** New
  `GET /api/v1/defaults` endpoint (`handle_defaults`) writes
  compiled-in defaults to disk and triggers reinit.  WebUI JS
  `restoreDefaults()` rewired from `/api/v1/restart` to the new
  endpoint (embedded gzip regenerated).  Previously the button just
  reloaded the on-disk config — misleading, and did nothing if the
  file already matched in-memory state.
- **Fix #5 — IDR on bitrate change.** `apply_bitrate` in both
  `src/star6e_controls.c` and `src/maruko_controls.c` now issues
  `MI_VENC_RequestIdr` after `MI_VENC_SetChnAttr`, gated through the
  existing `idr_rate_limit_allow` so storm callers stay coalesced.
  The decoder now gets a fresh keyframe to resync against the new
  rate-control state instead of drifting on stale P-frames.
- **Fix #4 — image.mirror / image.flip** deferred to hardware
  verification.  The reinit + save path is now reachable (fixes #1
  and #2), so a set+restart round-trip should apply these.  No code
  change this PR; follow-up on 192.168.1.13.

## [0.7.7] - 2026-04-18

- **Perf-series PR-C.1 — port MI_VENC_GetFd + poll() blocking wait to
  the Maruko main encoder loop.** Follow-up to PR-C (Star6E dual_rec
  only).  Maruko's main encoder loop in `maruko_pipeline_run`
  (`src/maruko_pipeline.c:1291`) was spinning on `maruko_mi_venc_query
  + usleep(500)` — ~2000 syscalls/s during idle gaps.  Replaced with
  `poll(MI_VENC_GetFd, 1000 ms)` and a wall-clock idle-abort timer
  (20 s of no frames → abort, preserved from the original
  `idle_counter * 500us` logic).
- **Fallback preserved:** if `MI_VENC_GetFd` returns < 0 on an unknown
  BSP variant, the loop falls back to the original Query+usleep(500)
  spin.  POLLERR/POLLHUP/POLLNVAL on the fd path drops into the
  fallback for the rest of the run.
- **Lifecycle:** `MI_VENC_CloseFd` called at cleanup when the fd was
  acquired.  The fd function pointers were already loaded by
  `maruko_mi.c` (dlsym'd but unused before this PR).
- **New bindings** (`include/maruko_bindings.h`): `maruko_mi_venc_get_fd`
  and `maruko_mi_venc_close_fd` macros alongside the existing
  MI_VENC_* wrappers.
- **Wall-clock idle timeout** consolidates the old dual idle paths
  (500us-keyed counter) into a single `wb_monotonic_us()`-based
  deadline that works identically on both the fd path (rare wakeups)
  and the fallback spin path (frequent wakeups).

## [0.7.6] - 2026-04-18

- **Perf-series PR-C — dual_rec_thread blocking wait via MI_VENC_GetFd.**
  Third of the 2026-04-18 perf series.  Replaces the 1-ms `usleep` spin
  in the dual-recorder thread with a `poll()` on the VENC channel's
  kernel fd (`MI_VENC_GetFd`).  The fd signals `POLLIN` when a frame is
  ready, so the thread wakes once per frame (~120/s at 120 fps) instead
  of ~1000/s from the old 1 ms spin — ~88 % fewer syscalls during
  recording.
- **Fallback preserved:** if `MI_VENC_GetFd` returns < 0 on an unknown
  BSP variant, the thread falls back to the original
  `MI_VENC_Query + usleep(1000)` loop — zero behaviour change on SDKs
  that don't expose the fd.
- **Lifecycle:** `MI_VENC_CloseFd` is called on thread exit when the fd
  was acquired.  The fd function pointers were already loaded by
  `star6e_mi.c` / `maruko_mi.c` (dlsym'd but unused before this PR).

## [0.7.5] - 2026-04-18

- **Perf-series PR-B — IDR request rate-limit gate.** Second of the
  2026-04-18 perf series.  Addresses the latent stability hazard where
  five independent IDR producers (scene detector, HTTP `/request/idr`
  and `/api/v1/dual/idr`, controls-apply, recorder-start) could storm
  `MI_VENC_RequestIdr` without coordination — a bug-driven burst
  (mis-tuned scene threshold during a camera pan) can crater per-frame
  bitrate by chaining forced keyframes.
- **New module (`include/idr_rate_limit.h`, `src/idr_rate_limit.c`):**
  per-channel (up to 8) last-honored timestamp + honored/dropped
  counters.  `idr_rate_limit_allow(chn)` enforces a compile-time
  `IDR_RATE_LIMIT_MIN_SPACING_US` of 100 ms — at 120 fps that is 12
  frames between honored forced IDRs, well below the GOP period
  (~83 ms at GOP=10, which auto-inserts an IDR without RequestIdr).
  State is lock-free (`__atomic_` load/store on `uint64_t`/`uint32_t`).
- **Wired through the 5 producer sites:**
  - `src/star6e_runtime.c` — `star6e_scene_request_idr`,
    `runtime_request_idr`
  - `src/star6e_controls.c` — `request_idr` (backend callback for HTTP
    `/request/idr`)
  - `src/venc_api.c` — `handle_dual_idr` (HTTP `/api/v1/dual/idr`);
    coalesced response returns `{"coalesced":true}`
  - `src/maruko_pipeline.c` — `maruko_scene_request_idr`
  - `src/maruko_controls.c` — `apply_qp_delta` IDR reissue
- **New endpoint `GET /api/v1/idr/stats`** returns per-channel honored
  and dropped counts plus the configured `min_spacing_us`.  Used by
  `tools/idr_storm.sh` to validate the gate.
- **Unit tests (`tests/test_idr_rate_limit.c`, 20 cases):** first-call
  honored, burst coalescing, per-channel independence, out-of-range
  bypass, post-spacing honored, reset semantics.  1139 tests pass
  (up from 1119).

## [0.7.4] - 2026-04-18

- **Perf-series PR-A — clock wrapper + dual_rec Query dedup + bench infra.**
  First of a five-PR series landing the post-review performance findings
  from 2026-04-18 (see `bench/perf-series/README.md`).
- **Clock reads via vDSO (`include/timing.h`, `src/timing.c`):** New
  `wb_monotonic_us()` helper using `CLOCK_MONOTONIC` (vDSO fast path on
  ARMv7, ~100 ns/call) instead of `CLOCK_MONOTONIC_RAW` (real syscall,
  ~1500 ns/call on A7).  Replaces three duplicated local wrappers —
  `monotonic_us` in `star6e_video.c` and `maruko_pipeline.c`, and
  `now_us` in `rtp_sidecar.c`.  NTP slew is <500 ppm → <4 us drift over
  a 60 s bench window, well inside frame-timing measurement error.
- **dual_rec backpressure signal (`src/star6e_runtime.c`):** Replaced the
  post-`MI_VENC_ReleaseStream` peek `MI_VENC_Query` with an inspection of
  the pre-`GetStream` `stat.curPacks >= 2` condition.  Equivalent
  semantics (queue had a backlog before we consumed) at one fewer syscall
  per recorded frame (~120/s at 120 fps).
- **Perf-series bench harness (`bench/perf-series/`):** New
  `run_bench.sh` drives the Tier A/B/C bench recipe end-to-end (deploy,
  probe, collect); `compare.py` emits a markdown Delta table between two
  labels with a 1.5×sigma regression flag.  Baseline tag
  `perf-series-baseline` pinned at master `40b8435`.
- **Host microbench (`tools/clock_bench.c`):** 1 M-iteration loop over
  `CLOCK_MONOTONIC_RAW`, `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_COARSE` to
  validate the vDSO assumption on A7 before deploying the PR.
- **IDR-storm stress (`tools/idr_storm.sh`):** Infrastructure for PR-B
  validation; fires N `POST /api/v1/dual/idr` back-to-back and reports
  the honored:fired ratio.

## [0.7.3] - 2026-04-14

- **Star6E sidecar gate (parity with Maruko PR #37):** Gated the per-frame
  `rtp_sidecar_poll` / `monotonic_us` / `rtp_sidecar_send_frame` work in
  `star6e_video_send_frame` on `state->sidecar.fd >= 0`.  When the
  sidecar feature is disabled (port 0), these calls are now skipped
  entirely rather than relying on each callee's early return.
- **SHM write: iovec-style 3-segment ring put (`venc_ring.h`, both backends):**
  Added `venc_ring_write3(hdr, p1, p2)` so the producer no longer has to
  pre-flatten `payload1 + payload2` into an 8 KB `flat[]` stack buffer
  before calling `venc_ring_write`.  Drops one memcpy per fragmented RTP
  packet (H.265 FU), removes the 8 KB stack allocation, and eliminates
  the `RTP_BUFFER_MAX` size clamp on the SHM write path.
  Applied to `src/star6e_output.c::star6e_output_send_rtp_parts` and
  `src/maruko_video.c::maruko_video_send_rtp_parts`.
  `venc_ring_write` is preserved as a thin wrapper for existing callers
  (C and C++, including the wfb_tx patched consumer).

## [0.7.1] - 2026-04-12

- **Phase 5 — Maruko HEVC RTP parity (PR #32):** Extracted the HEVC RTP
  output stage into a shared `hevc_rtp` module (`include/hevc_rtp.h` +
  `src/hevc_rtp.c`). Both Star6E and Maruko now go through the same
  Aggregation Packet (type 48) builder, FU-A fragmentation, VPS/SPS/PPS
  prepend-on-IDR, and per-frame `HevcRtpStats`. `star6e_hevc_rtp.c` is
  now a thin stream-iteration wrapper (227 lines → 111 lines);
  `Star6eHevcRtpStats` becomes a typedef alias of `HevcRtpStats` so
  existing call sites are unchanged. Maruko's RTP output gets standards-
  compliant AP aggregation for the first time: hardware-validated on
  SSC378QE at H.265 CBR 118 fps / 8 Mbps — IDR frames pack
  VPS+SPS+PPS+IDR as a single AP packet (`ap 1/6` in `[pktzr]` verbose
  line) instead of 4 separate RTP datagrams.
- **`[pktzr]` verbose line on Maruko:** Matches Star6E's exact format
  (`nals N | rtp N | fill N B | single N | ap N/N | fu N`) so log
  tooling works across both backends.
- **H.264 RTP output removed from Maruko:** Maruko ships H.265-only on
  the RTP wire path. The H.264 path was never hardware-verified and
  Maruko's FPV use case is H.265 exclusive. Channel creation still
  accepts `codec=h264` for forward compatibility, but the frame sender
  emits a warning and drops output. Net -~130 lines in `maruko_video.c`.
- **New `test_hevc_rtp` suite** (3 tests, 16 assertions): AP packing of
  small NALs, AP→FU-A fallback on oversized NALs, VPS/SPS/PPS prepend
  behavior — uses a capture-callback harness (no sockets) so tests run
  in <1 ms. Existing Star6E AP/FU-A test still passes unchanged as
  regression guard.

## [0.7.0] - 2026-04-11

- **dlopen migration (both backends):** Both Star6E and Maruko now load all
  MI vendor libraries (SYS, VIF, VPE, VENC, ISP, SCL, SNR) at runtime via
  dlopen/dlsym instead of direct linking. Function pointers are dispatched
  through `_impl` structs (`g_mi_sys`, `g_mi_vif`, etc.) with macro wrappers
  so call sites are unchanged. Three-way preprocessor guards
  (`PLATFORM_STAR6E` / `PLATFORM_MARUKO` / test stubs) keep all paths clean.
  - Star6E: dependency-ordered loading (cam_os_wrapper → SYS → ISP/CUS3A
    with RTLD_LAZY for circular deps → VIF/VPE/SNR/VENC with RTLD_NOW).
  - Maruko: eliminated uClibc shim and 3+ MB of redundant libs on device.
  - New files: `star6e_mi.h/.c`, `maruko_mi.h/.c`.
  - Removed: `-lmi_*` link flags, `MARUKO_UCLIBC_DIR`, shim build rules.
- **Maruko IQ parameter system:** Full 60-parameter ISP image quality API
  for Maruko (Phase 2), matching Star6E's existing IQ support. Includes
  multi-field struct params (colortrans, r2y, OBC, etc.), dot-notation
  set, and export/import. New files: `maruko_iq.h/.c`.
- **Maruko sensor mode diagnostics:** Auto-cap exposure to sensor FPS
  for reliable 120fps cold-boot. Fix SCL clock configuration. Gain
  control and exposure callback improvements.
- **Disable AF in CUS3A:** Fixed-focus cameras (IMX415) no longer trigger
  AF motor init errors. All CUS3A enable sequences changed from
  `{1,1,1}` (AE+AWB+AF) to `{1,1,0}` (AE+AWB only). Post-override
  after EnableUserspace3A which internally re-enables AF.
- **Star6E VPE exit(127) fix:** Under dlopen, vendor MI_VPE_DisablePort
  calls exit(127) on non-existent channel. Fixed by probing channel
  with MI_VPE_GetChannelAttr before VPE teardown.
- **Bool cast safety:** MI_SNR_GetPlaneMode vendor function writes 4 bytes
  through a `_Bool*` pointer. Fixed with temp-int wrappers on both backends.
- **Known issues documented:** Maruko encoder stall after output
  disable/re-enable (`documentation/KNOWN_ISSUES.md`).
- **Build cleanup:** Removed dead `snr_toggle_test` and `snr_sequence_probe`
  build recipes (unbuildable without direct MI linking). Removed stale
  uClibc references from deploy scripts and docs.
- Added `sensors-src` submodule pointing to OpenIPC/sensors for sensor
  driver source reference.
- Added IMX335 IQ profile (`iq-profiles/imx335_greg_fpvVII-gpt200.json`).

## [0.6.1] - 2026-04-03

- Fix cold-boot 54fps lock with legacyAe: call MI_SNR_SetFps during
  pipeline startup to force sensor timing compliance when CUS3A is not
  active.
- Fix sidecar telemetry NULL pointer: enriched encoder feedback was
  never sent (enc_ptr was NULL instead of &enc_info).
- Scene detector: saturate frame_count to prevent EMA warmup re-entry
  after ~13h; cache frame_size/type to avoid redundant packet walks;
  skip spike logic entirely when disabled (threshold=0).
- Change video0.scene_threshold and scene_holdoff to MUT_RESTART
  (no live-apply pathway exists).
- Remove all enc_ctrl/encCtrl references from code and documentation.

## [0.6.0] - 2026-04-02

- Add inline scene detector in star6e_runtime.c (~150 lines) behind
  `video0.scene_threshold` config field.
  - Tracks frame size EMA, computes complexity (0-255).
  - Detects spikes above configurable threshold for holdoff consecutive frames.
  - Waits for spike to subside before requesting IDR (when threshold>0).
  - Two config fields: `video0.scene_threshold` (uint16, 0=off, 150=1.5x EMA
    spike detection), `video0.scene_holdoff` (uint8, default 2).
  - Default off (`scene_threshold=0`): no IDR injection — zero-risk default.
- Enrich RTP timing sidecar with per-frame encoder telemetry:
  `frame_type`, `complexity`, `scene_change`, `idr_inserted`,
  `frames_since_idr`.
- Add multi-field set to HTTP API: `GET /api/v1/set?a=1&b=2` applies
  multiple live fields atomically in one request.
- Add field capabilities endpoint with backend-specific support filtering:
  `GET /api/v1/capabilities` reports mutability and per-backend support.
- API improvements: camelCase alias table for Majestic-compatible clients,
  duplicate-field rejection in multi-set, mixed live/restart rejection.

## [0.5.0] - 2026-04-01

- Add debug OSD overlay for encoder diagnostics and EIS crop visualization.
  Disabled by default (`debug.showOsd`), zero runtime cost when off.
  - MI_RGN canvas overlay via dlopen — ARGB4444 pixel format, full-frame canvas
    with dirty-rect tracking (only clears/draws changed areas per frame).
  - Stats panel (top-left): fps counter, CPU% from /proc/stat, 3x scaled 8x8
    bitmap font with semi-transparent background.
  - EIS crop visualization (bottom-right): 1/3 scale miniature showing sensor
    area (white), margin boundary (yellow), and moving crop window (green fill).
  - NEON-accelerated row fill (vst1q_u16, 8 pixels per store, 2.4x vs naive).
  - Mutually exclusive with waybeam-hub `mod_osd_render` — both use MI_RGN
    global state on VPE channel 0.
  - Config: `"debug": { "showOsd": true }`, API: `debug.show_osd` (MUT_RESTART).
  - New files: `include/debug_osd.h`, `src/debug_osd.c`.

## [0.4.1] - 2026-03-27

- Fix IMU webui fields invisible: rename config keys `sampleRate` →
  `sampleRateHz`, `gyroRange` → `gyroRangeDps` to match dashboard SECTIONS.
- Add 5 missing default config keys: `eis.mode`, `record.bitrate`,
  `record.fps`, `record.gopSize`, `record.server`.
- Fix camelCase capabilities lookup for `swapXY` and `maxMB` in webui.
- Remove legacy `sendFeedback` outgoing config alias.
- Document config/webui/API four-layer sync rules in AGENTS.md.
- Fix cold-boot sensor framerate lock: poll ISP exposure limits up to 500 ms
  instead of skipping the shutter cap when struct is all-zero. Apply synthetic
  gain defaults as fallback so AE cannot converge on exposure > frame period.
- Add IQ enable/disable toggle: virtual `.enabled` field for non-bool params
  (e.g. `colortrans.enabled=0`). Import respects `enabled` JSON field.
  Dashboard shows toggle switch in expanded form for applicable params.

## [0.4.0] - 2026-03-22

- Add built-in web dashboard at `/` with Settings, API Reference, and
  Image Quality tabs. Served as pre-compressed gzip (14KB on the wire).
- Add multi-field IQ parameter descriptors: colortrans (3 offsets + 3x3
  matrix), r2y, obc, demosaic, false_color, crosstalk, wdr_curve_adv now
  expose all sub-fields via dot-notation set API and `"fields"` JSON object.
- Add IQ export/import: `GET /api/v1/iq` exports all 62 ISP params as JSON,
  `POST /api/v1/iq/import` restores them. Partial imports supported.
- Add all missing config sections to the API: record (including dual channel
  bitrate/fps/gopSize/server), EIS (12 params), IMU (7 params), full audio
  (6 params), and ISP extras (legacyAe, aeFps). Total: 75 controllable fields.
- Add FT_FLOAT field type for EIS float params with `%.6g` precision to
  prevent artifacts like `0.001` displaying as `0.0010000000474974513`.
- Add FT_UINT8 field type for `imu.i2c_addr` — fixes memory corruption where
  `FT_UINT` wrote 4 bytes to a 1-byte field.
- Consolidate frame-loss threshold into shared function with minimum 512 kbit/s
  absolute margin for low-bitrate streams and 200 Mbps overflow clamp.
- Add `g_iq_mutex` for thread-safe IQ query/set operations.
- Add `g_dual_mutex` for thread-safe dual channel HTTP handlers.
- Fix `#ifdef` to `#if HAVE_BACKEND_STAR6E` in dual_apply_bitrate (Maruko
  link error from upstream PR #18).
- Fix stream_packs memory leak in SIGHUP reinit path.
- Fix diagnostic JSON trailing comma when dlsym lookups partially resolve.
- Add snprintf overflow protection (`JSON_CLAMP` macro) in IQ query output.
- Add EINTR handling in httpd read loops.
- Move dual channel settings from raw JSON file parsing to VencConfigRecord
  struct fields, simplifying star6e_runtime.c.
- Increase HTTPD_MAX_ROUTES to 64, HTTPD_MAX_BODY to 8192.

## [0.3.4] - 2026-03-22

- Refresh the Star6E frame-loss threshold on live bitrate changes so
  `/api/v1/set?video0.bitrate=...` keeps frame dropping aligned with the
  updated main-channel bitrate.
- Refresh the Star6E dual-channel frame-loss threshold on
  `/api/v1/dual/set?bitrate=...` so ch1 live bitrate changes keep the same
  overflow protection policy as channel creation.

## [0.3.3] - 2026-03-18

- Add Opus audio codec via `libopus.so` (loaded at runtime; graceful fallback
  to PCM if absent). RTP payload type PT=120, 48kHz nominal clock per RFC 7587.
- Fix 48kHz audio on SSC338Q — three root causes:
  - I2S clock misconfiguration: `i2s.clock` must be `0` (MCLK disabled; I2S
    master generates clock from internal PLL). Setting clock=1 caused hardware
    to deliver 16kHz data regardless of `rate` field. Source: SDK reference
    `audio_all_test_case.c` which uses `eMclk=0, bSyncClock=TRUE`.
  - Ring buffer too small: `AUDIO_RING_PCM_MAX` was 1280 (16kHz stereo
    headroom). 48kHz mono frames are 1920 bytes; silent truncation produced
    invalid Opus frame sizes → `OPUS_BAD_ARG`. Increased to 3840 (48kHz
    stereo 20ms = 960×2×2).
  - `bSyncClock` was 0; set to 1 per SDK reference.
- Fix stdout filter not active on SIGHUP reinit: `stdout_filter_start()` was
  inside `start_ai_capture()` which is skipped when AI device persists across
  reinit. Moved to `star6e_audio_init()` to run on every init cycle.
- Fix `stdout_filter_stop()` ordering: `close(pipe_read)` moved after
  `pthread_join` to avoid closing the fd while the filter thread may still
  be reading from it.
- Add `stdout_filter_stop()` to fail path and libmi_ai unavailable early-return
  to prevent filter leaks on audio init failure.
- Remove dead `star6e_audio_clock_for_rate()` function.
- Increase DMA ring: `frmNum` 8→20 (400ms), prevents data loss under ISP/AE
  preemption bursts.
- Reduce output port depth to `user=1, buf=2` (was 2,4), saving ~40ms latency.
- Audio init survives SIGHUP reinit: AI device/channel state is persisted in
  `g_ai_persist` across reinit cycles to avoid `CamOsMutexLock` deadlock after
  2+ VPE create/destroy cycles.

## [0.3.2] - 2026-03-17
- Fix SIGHUP reinit D-state: switch from full pipeline_stop/start to partial
  teardown that keeps sensor/VIF/VPE running. The SigmaStar MIPI PHY does not
  recover from MI_SNR_Disable/Enable cycles — partial teardown avoids touching
  it entirely. VENC, output, audio, IMU/EIS are torn down and rebuilt; the
  VIF→VPE REALTIME bind stays active across reinit.
- Live resolution switching: `video0.size` API change now reconfigures the
  pipeline in-process without a process restart.
  - Same-aspect-ratio changes (e.g. 1920x1080 → 1280x720): VPE output port
    resize only — VIF and VIF→VPE bind are untouched.
  - Aspect-ratio changes (e.g. 1920x1080 → 1920x1440): full VIF crop
    reconfiguration + VPE destroy/recreate. VIF device stays running;
    MIPI PHY is never touched.
  - Overscan correction applied during reinit precrop: uses `mode.output`
    (usable area) rather than `plane.capt` (raw MIPI frame) for sensors that
    report overscan in the MIPI frame dimensions.
- Guard VIF→VPE bind in `bind_and_finalize_pipeline` to prevent double-bind
  on reinit. Without the guard, re-binding an already-live VIF→VPE port
  caused continuous `IspApiGet channel not created` dmesg errors.
- ISP channel readiness poll (`star6e_pipeline_wait_isp_channel`) called
  immediately after every new VIF→VPE bind. The ISP channel initialises
  asynchronously after `MI_VPE_CreateChannel`; the poll (up to 2000 ms,
  1 ms intervals) ensures the ISP is ready before the bin load and exposure
  cap APIs probe it, eliminating `IspApiGet` dmesg errors on both cold boot
  and AR-change reinit.
- `__attribute__((flatten))` on `star6e_pipeline_reinit`: forces GCC -Os to
  inline all static callees, preserving the stack layout that the SigmaStar
  ISP driver requires for `MI_VPE_CreateChannel` to succeed.
- Error-path state consistency in VIF+VPE reconfiguration: on failure after
  VPE is destroyed, `MI_VIF_DisableDev` is called to leave the pipeline in a
  cleanly-stopped state rather than a partially-configured one.
- Details: `documentation/SIGHUP_REINIT.md`

## [0.3.1] - 2026-03-16
- Reduce G.711 audio latency: scale frame size to `sample_rate/50` (~20ms)
  instead of hardcoded 320. Reduce MI_AI ring (frmNum 16→8), output port
  depth (4,16)→(2,4), fnGetFrame timeout 100→50ms.
- Add dynamic RTP payload types: PT=112 (PCMU non-8kHz), PT=113 (PCMA
  non-8kHz). Standard PTs (0, 8, 11) still used when rate matches RFC 3551.
- Clamp audio sample_rate to 8000-48000 in config parser.
- Default audio codec changed from `pcm` to `g711a` in venc.default.json.
- Remove `slicesEnabled`/`sliceSize`/`lowDelay` config fields (no firmware support on I6E).
- Add `frameLost` config field for frame-lost strategy (default: true).
- Fix kbps verbose overflow on 32-bit ARM (displayed ~400 instead of ~13000 at high bitrates).

## [0.3.0] - 2026-03-15
- Custom 3A thread for Star6E — replaces ISP internal AE/AWB with a
  dedicated 15 Hz thread (default, no config change needed):
  - AE: proportional controller with shutter-first priority, configurable
    target luma (100-140), convergence rate (10%), and gain ceiling (20x).
  - AWB: grey-world algorithm with IIR smoothing (70/30) and 2% dead-band.
  - Pauses ISP AE via `MI_ISP_AE_SetState(PAUSE)`, disables CUS3A AWB
    callback via `MI_ISP_CUS3A_Enable(1,0,0)`.
  - Periodic ISP AE state verification with automatic re-pause.
  - Manual AWB (`ct_manual`) pauses custom AWB; `auto` resumes it.
  - `isp.exposure` API syncs max shutter to the custom AE thread.
  - Set `isp.legacyAe: true` to revert to old ISP AE + handoff behavior.
- New config fields: `aeFps`, `legacyAe` in the `isp` section.
  Gain/shutter limits now seeded from ISP bin (`MI_ISP_AE_GetExposureLimit`).
- HW verified: all 4 imx335 sensor modes (30/60/90/120fps), cold-boot,
  live FPS switching, gemini dual recording, manual AWB transitions.

## [0.2.3] - 2026-03-14
- Restored working Star6E AE across IMX335 modes `30`, `60`, `90`, and `120 fps`:
  - Startup now primes CUS3A with `100 -> 110 -> 111`.
  - Steady state no longer forces periodic `110` refreshes.
  - A delayed one-shot `000` handoff returns the pipeline to a live AE state
    while preserving the requested encoder rate.
- Added Star6E AE diagnostics for live verification:
  - `GET /api/v1/ae`
  - `GET /metrics/isp`
  - Existing `GET /api/v1/awb` remains available for AWB inspection.
- Documented the verified AE recovery and updated the HTTP API contract for
  the diagnostics endpoints.

## [0.2.2] - 2026-03-11
- Fixed GOP keyframe interval to be relative to FPS (seconds, not raw frames):
  - `gopSize` is now a float representing seconds between keyframes.
  - `1.0` = 1 keyframe/second (GOP = fps frames). `0.5` = every 0.5s. `0` = all-intra.
  - Example: `gopSize: 0.33` at 90fps = keyframe every ~30 frames.
  - Changing FPS now automatically recalculates GOP frame count.
  - Default changed from `3` (frames) to `1.0` (seconds).
- Fixed autoexposure not restoring via HTTP API:
  - Setting `isp.exposure=0` via API now correctly restores auto-exposure
    (caps max shutter to frame period). Previously it was a no-op due to
    both args being zero in `cap_exposure_for_fps(0, 0)`.
- Known issue: AWB (Auto White Balance) behavior unverified on device.
  - CUS3A enables AWB (`params[1]=1`) but actual color correction depends on
    ISP bin calibration data. Requires on-device testing. See
    `documentation/AWB_INVESTIGATION.md`.
- Known issue: ROI QP not yet wired to encoder backend.
  - Config plumbing and HTTP API exist but `apply_roi_qp` callback is NULL.
  - SDK supports overlapping ROI regions with delta QP via
    `MI_VENC_SetRoiCfg`. Implemented as horizontal bands with signed QP
    (1-4 steps). See `documentation/ROI_INVESTIGATION.md`.

## [0.2.1] - 2026-03-10
- Added audio output via UDP with configurable codec and port:
  - Supported codecs: raw PCM, G.711 A-law, G.711 μ-law (software encoding).
  - Audio captured via MI_AI SDK (dlopen at runtime, graceful degradation if unavailable).
  - New `audio` config section: `enabled`, `sampleRate`, `channels`, `codec`, `volume`, `mute`.
  - New `outgoing.audioPort` field: 0 = share video port, >0 = dedicated audio port (default 5601).
  - Audio runs in a separate thread from the video streaming loop.
  - Dual packetization: compact mode (0xAA magic header) and RTP mode (PT 110, distinct SSRC).
  - Live mute/unmute via HTTP API (`audio.mute`, MUT_LIVE).
  - Star6E backend: full implementation. Maruko backend: warning stub.
- RTP mode now reads `maxPayloadSize` from config (was hardcoded to 1200):
  - Both star6e and maruko backends respect `outgoing.maxPayloadSize` for
    RTP FU-A/FU fragmentation threshold. Default 1400.
  - Config values above 1400 are supported for jumbo-frame networks.
- Added adaptive RTP payload sizing to reduce CPU churn from packet overhead:
  - EWMA tracks average P-frame size; IDR-like spikes (>3x average) are
    excluded to prevent distortion.
  - Target payload = avg_frame * fps / targetPacketRate, aiming for ~850
    packets/sec by default across all bitrates (adaptive bitrate up to 50 Mbit).
  - `outgoing.targetPacketRate` config field (default 850, MUT_RESTART).
    Set to 0 to disable adaptive sizing and use fixed maxPayloadSize.
  - 15% hysteresis prevents oscillation on frame-to-frame jitter.
  - Payload clamped to [1000, maxPayloadSize]. The 1000-byte floor keeps
    packet rate under ~500 pkt/s on low-MCS WiFi links (MCS0 slot budget).

## [0.2.0] - 2026-03-10
- Added output enable/disable control (`outgoing.enabled`, MUT_LIVE):
  - When disabled: FPS reduces to 5fps idle rate, frames encoded and discarded.
  - When enabled: FPS restores to previous value, IDR keyframe issued.
  - Default: `false` (no more implicit localhost:5000 fallback).
- Added live destination redirect (`outgoing.server`, MUT_LIVE):
  - Change UDP destination without pipeline restart.
  - IDR keyframe issued on destination change for stream continuity.
  - Re-connects UDP socket when `connectedUdp` is enabled.
- Added stream mode config field (`outgoing.streamMode`, MUT_RESTART):
  - Values: `"rtp"` (default) or `"compact"`.
  - Replaces scheme-derived mode detection; URI scheme must be `udp://`.
- Added connected UDP (`outgoing.connectedUdp`, MUT_RESTART):
  - When true: `connect()` called on UDP socket, skips per-packet routing
    lookup and enables kernel ICMP error feedback.
- Added IDR request after live bitrate change for immediate quality update.
- Updated HTTP API contract to v0.2.0.

## [0.1.7] - 2026-02-26
- Fixed ISP FIFO stall on overscan sensor modes (imx335 mode 2 @ 90fps):
  - Added periodic CUS3A refresh (~15 Hz) in stream loop to keep ISP event
    loop alive; runs in both idle and active paths so a stalled pipeline
    can recover.
  - Fixed overscan detection: removed 10% threshold that skipped correction
    for single-axis overscan (imx335 mode 2: crop 2560x1440, output 2400x1350).
    Changed to per-axis independent clamping.
- Simplified ISP 3A management (Star6E + Maruko):
  - Replaced per-frame AE cadence toggling and ISP3AHandle/ISP3AState machinery
    with one-shot `enable_cus3a()` at pipeline init + periodic `cus3a_tick()`.
  - Removed CLI flags: `--ae-on/off`, `--awb-on/off`, `--af-on/off`, `--ae-cadence`.
- Added ISP/SCL clock boost (384 MHz) after pipeline setup.
- Added `--oc-level` for hardware overclocking:
  - Level 1: VENC clock boost to 480 MHz.
  - Level 2: Level 1 + CPU pinned to 1200 MHz with performance governor.

## [0.1.6] - 2026-02-25
- Added AE cadence control (`--ae-cadence N`) for high-FPS throughput recovery:
  - Toggles CUS3A processing on/off every N frames to reduce per-frame CPU overhead.
  - Auto mode: when FPS >60, cadence defaults to fps/15 (e.g. cadence=8 at 120fps).
  - Manual override via `--ae-cadence N` for fine-tuning.
- Moved ISP bin load earlier in pipeline setup (after start_vpe, before streaming)
  to ensure correct ae_init state before first frame.
- Added overscan crop detection for sensor modes where mode.output < mode.crop:
  - When overscan exceeds 10% on both axes, VIF center-crops to the usable output area.
  - Fixes imx415 mode 1 hang (crop=2952x1656, output=2560x1440).
  - Threshold prevents false positives from driver metadata quirks.
- Enhanced `--list-sensor-modes` to show crop/output details when they differ.
- Cleaned up pipeline summary prints: explicit MIPI frame vs cropped dimensions,
  precrop line only shown for actual aspect-ratio cropping.

## [0.1.5] - 2026-02-25
- Improved agentic coding workflow in AGENTS.md:
  - Added structured error recovery loop (observe → diagnose → repair → re-verify → document).
  - Added incremental verification guidance: run `make lint` after each logical change.
  - Added long-session guidance: progress checkpoints, decision stability, scope control.
  - Added error diagnosis reference table for compiler, linker, runtime, and timeout failures.
  - Added deployment test interpretation: exit codes, JSON summary, dmesg guidance, agent decision flow.
  - Added "Mistakes to Avoid" entries for stacking unverified changes and mid-task approach switching.
- Added `make lint` target: fast compile-only check with `-Wall -Wextra -Werror` for both backends.
- Added lint step to CI workflow (runs before build).
- Synced dual-agent infrastructure (Claude Code + OpenAI Codex):
  - Updated all `.agents/skills/` and `.claude/commands/` with decision documentation,
    error recovery loop, and incremental lint steps.
  - Added `Bash(make lint*)` to Claude permissions; switched PostToolUse hook
    from full build to fast lint for tighter feedback loop.
  - Enhanced `.codex/config.toml` with `sandbox_mode = "workspace-write"`.
- Improved `remote_test.sh`:
  - Added SSH ControlMaster multiplexing for persistent connections.
  - Removed runtime lib deployment (libs already in `/usr/lib` on target).
  - Added `--json-summary`, `--skip-build`, `--skip-deploy` flags.
  - Added strict exit codes (0=success, 1=failed, 2=unresponsive, 124=timeout).
- Added `documentation/TARGET_AGENT_ARCHITECTURE.md` design doc (deferred implementation).

## [0.1.4] - 2026-02-23
- Added automatic precrop for Star6E: when encode resolution has a different aspect ratio
  than the sensor mode, the VIF center-crops the sensor frame to match the target aspect
  ratio before the VPE scales, eliminating non-uniform scaling distortion.
- Precrop uses integer cross-multiplication (no floats) with 2-pixel alignment enforcement.
- Informational log line printed when precrop is active (e.g. `Precrop: 1920x1080 -> 1440x1080 (offset 240,0)`).
- Fixed high-FPS throttling when AE is disabled: caps exposure to frame period after ISP bin
  load, preventing default 10ms shutter from limiting 120fps mode to ~99fps.

## [0.1.3] - 2026-02-23
- Added duplicate-process guard: venc now detects and exits if another instance is already running.
- Added `--version` / `-v` flag to print version and backend name.
- Added `--verbose` flag to gate per-frame stats output (previously always printed).
- Removed obsolete HiSilicon/Goke `-v [Version]` hardware presets from Star6E backend and help text.
- Simplified sensor mode selection: prioritize FPS match over resolution fit in both backends.
- Fixed Star6E cleanup ordering: socket and ISP 3A handle now properly released on all exit paths.
- Added informational prints for FPS mismatch, resolution clamping, and VPE scaling.
- Embedded build-time version from VERSION file via Makefile (`VENC_VERSION`).
- Updated help text branding from "HiSilicon/Goke" to "SigmaStar".
- Added crash/hang tracking policy and initial crash log (`documentation/CRASH_LOG.md`).
- Added SigmaStar Pudding SDK API reference link to proc reference and documentation index.

## [0.1.2] - 2026-02-22
- Added low-risk ISP CPU-control knobs in both standalone backends:
  - `--ae-off/--ae-on`
  - `--awb-off/--awb-on`
  - `--af-off/--af-on` (default AF off)
  - `--vpe-3dnr 0..7`
- Updated ISP bin load/reapply behavior to honor requested AE/AWB/AF state.
- Added documentation for CPU/latency tuning profiles and usage:
  - `documentation/AE_AWB_CPU_TUNING.md`
- Updated status/index docs to reflect implemented 3A/3DNR tuning controls.

## [0.1.1] - 2026-02-22
- Added formal HTTP API contract source-of-truth document:
  - `documentation/HTTP_API_CONTRACT.md`
- Added repository PR checklist template with explicit contract/version/doc gates:
  - `.github/pull_request_template.md`
- Added default JSON config template and planning artifacts for config/API migration:
  - `config/venc.default.json`
  - `documentation/CONFIG_HTTP_API_ROADMAP.md`
- Updated documentation/plan/process files to enforce:
  - Star6E-first rollout for SigmaStar API-touching features,
  - contract sync on HTTP changes,
  - SemVer + changelog workflow.

## [0.1.0] - 2026-02-22
- Baseline established for standalone-only repository scope.
- Targeted dual-backend builds in place (`SOC_BUILD=star6e`, `SOC_BUILD=maruko`).
- Runtime SoC autodetect removed from `venc`; backend is selected at build time.
- Default stream behavior aligned to RTP + H.265 CBR.
- Planning updates introduced for JSON config migration and HTTP control API roadmap.
