# History

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
- New config fields: `aeFps`, `aeTargetLow`, `aeTargetHigh`, `aeChangePct`,
  `aeGainMax`, `legacyAe` in the `isp` section.
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
  - Re-connects UDP socket when `sendFeedback` is enabled.
- Added stream mode config field (`outgoing.streamMode`, MUT_RESTART):
  - Values: `"rtp"` (default) or `"compact"`.
  - Replaces scheme-derived mode detection; URI scheme must be `udp://`.
- Added connected UDP feedback (`outgoing.sendFeedback`, MUT_RESTART):
  - When true: `connect()` called on UDP socket, kernel returns ICMP errors.
  - Enables detection of unreachable destinations via `sendmsg()` errors.
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
  - `star6e-standalone/config/venc.default.json`
  - `documentation/CONFIG_HTTP_API_ROADMAP.md`
- Updated documentation/plan/process files to enforce:
  - Star6E-first rollout for SigmaStar API-touching features,
  - contract sync on HTTP changes,
  - SemVer + changelog workflow.

## [0.1.0] - 2026-02-22
- Baseline established for standalone-only repository scope (`star6e-standalone/`).
- Targeted dual-backend builds in place (`SOC_BUILD=star6e`, `SOC_BUILD=maruko`).
- Runtime SoC autodetect removed from `venc`; backend is selected at build time.
- Default stream behavior aligned to RTP + H.265 CBR.
- Planning updates introduced for JSON config migration and HTTP control API roadmap.
