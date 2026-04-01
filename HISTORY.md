# History

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
