# Current Status And Next Steps

## Current Status
- Repository scope is standalone-only: `star6e-standalone/`.
- Build entrypoint is root `Makefile` with explicit SoC split:
  - `make build SOC_BUILD=star6e`
  - `make build SOC_BUILD=maruko`
- Required Maruko runtime/link libraries are now vendored in-repo under:
  - `star6e-standalone/libs/maruko/`
- Dual-backend split is active via targeted builds only:
  - `SOC_BUILD=star6e`
  - `SOC_BUILD=maruko`
- Runtime SoC autodetect/override in `venc` was removed intentionally.
- JSON runtime implementation was attempted and rolled back.
  - reason: Star6E regression (`waiting for encoder data`, no stream output).
  - detailed notes: `documentation/JSON_CONFIG_ROLLBACK_NOTES.md`
- Current runtime interface is the known-good CLI path (no active JSON runtime wiring).
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
- Live FPS control via hardware bind decimation (v0.1.7):
  - `video0.fps` API changes rebind VPE→VENC with `sensor_maxFps:requested_fps` ratio.
  - VENC rate control `fpsNum` updated for correct bitrate allocation.
  - Clamped to current sensor mode max; mode switching requires process restart.
  - Sensor always set to mode maxFps (avoids IMX335 intermediate fps stall).
  - FPS-aware mode selection prefers modes whose maxFps is closest to target.
  - Details: `documentation/LIVE_FPS_CONTROL.md`
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
- Remote workflow defaults were updated:
  - default script host: `root@192.168.1.11` (Maruko bench)
  - Star6E bench currently used: `root@192.168.2.10`
  - stream destination host: `192.168.1.2`

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

## High-FPS Unlock Summary (Star6E)
- Sensor custom-command pre-latch sequence for cold-boot unlock is documented in:
  - `documentation/SENSOR_UNLOCK_IMX415_IMX335.md`
- Historical unlock experiments and rationale are tracked in:
  - `documentation/IMPLEMENTATION_PHASES.md`

## Prioritized Next Steps
1. ~~Introduce HTTP control API for live updates~~ — **done** (v0.1.7):
   - Read-only endpoints, live-safe writes (bitrate, fps, gop, exposure), and restart-required
     settings are all implemented. Live FPS uses hardware bind decimation.
   - Contract: `documentation/HTTP_API_CONTRACT.md` (v0.1.3).
2. Harden JSON config model:
   - reintroduce parser/runtime wiring in a dedicated branch only,
   - keep stream graph behavior unchanged during parser migration,
   - define strict/compatible behavior for unknown keys,
   - validate Star6E parity against CLI baseline before merge,
   - add config migration notes for future schema versions.
3. Precrop aspect-ratio correction for Maruko (SCL-level crop):
   - Port Star6E precrop logic to Maruko using `scl_port.crop`.
   - Design doc is ready: `documentation/PRECROP_ASPECT_RATIO.md`.
4. Extend 3A controls from on/off into cadence/profile tuning (Star6E-first):
   - **done:** `--ae-cadence N` implemented for Star6E (v0.1.6), auto mode at >60fps.
   - quantify CPU savings vs image adaptation behavior,
   - port stable controls to Maruko after Star6E validation.
5. Keep Star6E-first implementation order for SigmaStar API-touching changes:
   - validate on Star6E first, then port to Maruko.
6. Maintain regression gates on both boards after each merged change:
   - Star6E (`192.168.2.10`): CLI baseline run(s) with matching known-good settings.
   - Maruko (`192.168.1.11`): H.265 compact + H.265 RTP with `isp.bin_path=/etc/sensors/imx415.bin`.
7. Complete Maruko codec parity checks:
   - verify H.264 behavior (`264cbr`) end-to-end with current graph path.
8. Resume deferred Maruko sensor-depth work after driver refresh:
   - mode/fps mapping validation,
   - direct ISP-bin load stability,
   - >30fps capability checks.
