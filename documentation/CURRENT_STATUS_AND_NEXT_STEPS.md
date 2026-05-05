# Current Status And Next Steps

## Current Status
- Repository scope is the repo root.
- Build entrypoint is root `Makefile` with explicit SoC split:
  - `make build SOC_BUILD=star6e`
  - `make build SOC_BUILD=maruko`
- Required Maruko runtime/link libraries are now vendored in-repo under:
  - `libs/maruko/`
- Dual-backend split is active via targeted builds only:
  - `SOC_BUILD=star6e`
  - `SOC_BUILD=maruko`
- Runtime SoC autodetect/override in `venc` was removed intentionally.
- JSON config runtime is active:
  - `venc` loads `/etc/venc.json` and serves the HTTP API as the primary runtime interface.
  - current Star6E validation uses `scripts/star6e_direct_deploy.sh` against `root@192.168.1.13`.
  - historical rollback notes remain in `documentation/JSON_CONFIG_ROLLBACK_NOTES.md`.
- Automatic VIF precrop for Star6E is implemented (v0.1.4):
  - When encode resolution has a different aspect ratio than the sensor mode,
    VIF center-crops the sensor frame to match before VPE scaling.
  - Details: `documentation/PRECROP_ASPECT_RATIO.md`
- High-FPS exposure cap is implemented (v0.1.4):
  - After ISP bin load, `maxShutterUs` is capped to `1000000/fps` to prevent
    the default 10ms shutter from throttling high-fps modes (e.g. 120fps capped at ~99fps).
- Custom 3A thread for Star6E (replaces AE cadence):
  - Dedicated 15 Hz thread handles AE (proportional controller) and AWB (grey-world).
  - Pauses ISP internal AE, disables CUS3A AWB callback — full 3A control.
  - Default mode; set `isp.legacyAe=true` to revert to ISP AE + handoff.
  - Configurable: target luma, convergence rate, gain ceiling, processing rate.
  - Details: `documentation/AE_AWB_CPU_TUNING.md`
- Live reinit and resolution switching without process restart (v0.3.2):
  - SIGHUP (`killall -1`) and `/api/v1/restart` reload config and rebuild
    the pipeline in-process. The SigmaStar MIPI PHY is never cycled —
    sensor/VIF/VPE are preserved, only VENC/output/audio are rebuilt.
    Eliminates the previous D-state hang caused by `MI_SNR_Disable/Enable`.
  - `video0.size` API change reconfigures the pipeline live:
    - Same-AR resize (e.g. 1920x1080 → 1280x720): VPE output port update only.
    - AR change (e.g. 1920x1080 → 1920x1440): VIF crop + VPE destroy/recreate.
  - ISP channel readiness poll after every new VIF→VPE bind eliminates
    "IspApiGet channel not created" dmesg errors on start and AR-change reinit.
  - Sensor mode changes still require a full process restart.
  - Details: `documentation/SIGHUP_REINIT.md`
- Live FPS control via hardware bind decimation (v0.1.7):
  - `video0.fps` API changes rebind VPE→VENC with `sensor_maxFps:requested_fps` ratio.
  - VENC rate control `fpsNum` updated for correct bitrate allocation.
  - Clamped to current sensor mode max; mode switching requires process restart.
  - Sensor always set to mode maxFps (avoids IMX335 intermediate fps stall).
  - FPS-aware mode selection prefers modes whose maxFps is closest to target.
  - Details: `documentation/LIVE_FPS_CONTROL.md`
- Inline scene detector is now integrated into Star6E `venc` via `video0.scene_threshold`:
  - Tracks frame size EMA, computes complexity (0-255), detects scene change
    spikes above threshold, and requests IDR after the spike settles.
  - Two config fields: `video0.scene_threshold` (uint16, 0=off, 150=1.5x EMA
    spike detection), `video0.scene_holdoff` (uint8, default 2).
  - Reports `frame_type`, `complexity`, `scene_change`, `idr_inserted`,
    `frames_since_idr` via RTP timing sidecar when enabled.
  - hardware-validated on `root@192.168.1.13` in `cbr`, `vbr`, and `avbr`.
- Overscan crop detection for Star6E (v0.1.6):
  - When sensor mode.output < mode.crop by >10%, VIF center-crops to usable area.
  - Fixes imx415 mode 1 pipeline hang (crop=2952x1656, output=2560x1440).
- New low-risk ISP CPU knobs are now available in both backends:
  - `--ae-off/--ae-on`
  - `--awb-off/--awb-on`
  - `--af-off/--af-on` (default off)
  - `--vpe-3dnr 0..7` (default 1)
  - details: `documentation/AE_AWB_CPU_TUNING.md`
- Maruko backend status (on `openipc-ssc378qe`, family `infinity6c`):
  - end-to-end H.265 compact UDP stream works and emits frames.
  - end-to-end H.265 RTP mode works with visible frames.
  - ring-pool + graph setup changes are in place and stable in smoke runs.
- Remote workflow:
  - preferred Star6E `venc` validation target: `root@192.168.1.13`
  - `remote_test.sh` default host remains `root@192.168.1.11` (legacy Maruko bench), so pass `--host` explicitly
  - stream destination host on the 192.168.1.x bench: `192.168.1.2`

- SD card MPEG-TS recording with audio (Star6E):
  - HEVC video + PCM audio muxed into power-loss safe .ts container.
  - File rotation at IDR boundaries by time (default 300s) or size (default 500MB).
  - HTTP API: `/api/v1/record/start`, `stop`, `status`.
  - Concurrent with RTP streaming at 0-4% additional CPU overhead.
  - Config: `record.format` ("ts" or "hevc"), `record.dir`, `maxSeconds`, `maxMB`.
  - HW verified at 30fps and 120fps on Star6E imx335.
  - Details: `documentation/SD_CARD_RECORDING.md`

- Gemini mode — dual VENC (Star6E):
  - Four modes via `record.mode`: off, mirror, dual, dual-stream.
  - Dual mode: stream at 30fps via ch0, record at 120fps via ch1 simultaneously.
  - Dual-stream: two independent RTP outputs at different quality settings.
  - Dedicated recording thread for ch1 — prevents VPE backpressure at 120fps.
  - Adaptive bitrate: auto-reduces ch1 bitrate (10%/s) if SD card can't keep up.
  - HTTP API: `/api/v1/dual/status`, `set?bitrate=N`, `set?gop=N`, `idr`.
  - VPE SCL clock preset safety net in signal handlers for forced exit paths.
  - HW verified: 3 consecutive start/stop cycles, all modes, audio + dual coexistence.
  - Details: `documentation/SD_CARD_RECORDING.md` (Gemini Mode section)

## Known Limitations
- Current Maruko image/driver stack appears sensor/ISP constrained in some cases.
- Deep sensor-mode/high-FPS mapping on Maruko is deferred until newer driver update.
- Treat Maruko validation scope for now as stable 30fps streaming + backend correctness.

## High-FPS Unlock Summary (Star6E + Maruko)
- Sensor custom-command pre-latch sequence for cold-boot unlock is documented in:
  - `documentation/SENSOR_UNLOCK_IMX415_IMX335.md`
- Historical unlock experiments and rationale are tracked in:
  - `documentation/IMPLEMENTATION_PHASES.md`
- **Ported to Maruko** (`maruko_pipeline.c:930-933`): IMX415 / IMX335 cold-boot
  latch unlock runs identically on Maruko before sensor mode select.
- **Frame-lost overshoot protection** is shared via
  `pipeline_common_frame_lost_threshold()` (`src/pipeline_common.c`) and runs on
  both backends.  Threshold is target × 1.5 with a 512 kbps floor since v0.9.8;
  see `HISTORY.md` 0.9.8 entry for the motion-burst rationale.

## Prioritized Next Steps
1. ~~Introduce HTTP control API for live updates~~ — **done** (v0.1.7):
   - Read-only endpoints, live-safe writes (bitrate, fps, gop, exposure), and restart-required
     settings are all implemented. Live FPS uses hardware bind decimation.
   - Contract: `documentation/HTTP_API_CONTRACT.md` (v0.1.3).
2. Harden the live JSON config model:
   - keep schema validation behavior explicit (strict vs compatible handling of unknown keys),
   - keep direct helper, HTTP API, and on-disk config examples synchronized,
   - validate Star6E parity against the current `/etc/venc.json` runtime path,
   - add config migration notes for future schema versions.
3. ~~Precrop aspect-ratio correction for Maruko (SCL-level crop)~~ — **done**
   (v0.9.9): `configure_maruko_scl()` writes a centered rect via
   `pipeline_common_compute_precrop()`; see `documentation/PRECROP_ASPECT_RATIO.md`
   and `documentation/MARUKO_PARITY_PLAN.md` Phase 1.
4. Extend 3A controls from on/off into cadence/profile tuning (Star6E-first):
   - **done:** `--ae-cadence N` implemented for Star6E (v0.1.6), auto mode at >60fps.
   - quantify CPU savings vs image adaptation behavior,
   - port stable controls to Maruko after Star6E validation.
5. Keep Star6E-first implementation order for SigmaStar API-touching changes:
   - validate on Star6E first, then port to Maruko.
6. Maintain regression gates on both boards after each merged change:
   - Star6E (`192.168.1.13`): direct deploy cycle plus targeted `remote_test.sh` sweeps when sensor-mode coverage matters.
   - Maruko (`192.168.2.12`): H.265 compact + H.265 RTP with `isp.bin_path=/etc/sensors/imx415.bin`.
7. Complete Maruko codec parity checks:
   - verify H.264 behavior (`264cbr`) end-to-end with current graph path.
8. Resume deferred Maruko sensor-depth work after driver refresh:
   - mode/fps mapping validation,
   - direct ISP-bin load stability,
   - >30fps capability checks.
