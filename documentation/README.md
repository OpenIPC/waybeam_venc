# Documentation Index

This folder is the canonical place for waybeam_venc project documentation.

## Core Docs
- `documentation/DUAL_BACKEND_SPLIT_PLAN.md`
  - Current architecture/implementation plan for Star6E + Maruko targeted builds.
  - Tracks only remaining implementation steps from the current stable baseline.
- `documentation/CONFIG_HTTP_API_ROADMAP.md`
  - JSON config migration plan (`/etc/venc.json`) and HTTP runtime-control API scope.
  - Contains full settings inventory and mutability classes.
- `documentation/JSON_CONFIG_ROLLBACK_NOTES.md`
  - Postmortem notes from the rolled-back JSON runtime attempt.
  - Captures regression signatures and guardrails for the next implementation attempt.
- `documentation/HTTP_API_CONTRACT.md`
  - Canonical HTTP API contract (endpoints, payloads, status codes, mutability semantics).
  - Must be updated in the same PR as any HTTP API behavior change.
- `documentation/SENSOR_UNLOCK_IMX415_IMX335.md`
  - Reproducible cold-boot unlock sequence for IMX415/IMX335 high-FPS modes.
- `documentation/AE_AWB_CPU_TUNING.md`
  - Implemented AE/AWB/AF and 3DNR runtime knobs for CPU/latency tuning.
  - Includes current defaults and practical usage presets.
- `documentation/LIVE_FPS_CONTROL.md`
  - Live FPS control via hardware bind decimation (MI_SYS_BindChnPort2).
  - Covers clamping behavior, mode selection, tested configurations, and mode switching limitation.
- `documentation/DEBUG_OSD_PLAN.md`
  - Debug OSD overlay API reference. Covers enabling, drawing primitives (text,
    rect, point, line), color constants, and how to add debug output from new modules.
- `documentation/OPTFLOW_FUSION_ROADMAP.md`
  - Optical flow + IMU fusion implementation roadmap.
  - Covers derotation, flight controller integration (MAVLink/MSP), degraded modes,
    performance budget, and phased implementation plan.
- `documentation/REMOTE_TEST_WORKFLOW.md`
  - Standard remote build/upload/run workflow, cold-state rules, and hang handling.
- `documentation/CRASH_LOG.md`
  - Incident log for SoC crashes/hangs. Three-strike rule triggers deeper investigation.
- `documentation/IMPLEMENTATION_PHASES.md`
  - Detailed implementation-phase timeline and checkpoints from the debugging/refactor sessions.
  - Contains historical command references from earlier phases; use current build commands from this index/README.
- `documentation/CURRENT_STATUS_AND_NEXT_STEPS.md`
  - Short operational status and prioritized next actions.
  - Includes latest Maruko hardware smoke-test outcomes.
- `HISTORY.md`
  - Canonical changelog from the current baseline onward.
- `VERSION`
  - Current repository/application version (SemVer).

## SigmaStar SDK API Reference

- [Pudding SDK Documentation](https://wx.comake.online/doc/doc/SigmaStarDocs-Pudding-0120/) — covers SENSOR, VIF, VPE, VENC, SYS, and ISP modules used by this project.

## Scope
- Active implementation: `src/`
- All new implementation notes should be added under `documentation/`.
- Default JSON config template is maintained in:
  - `config/venc.default.json`
- `AGENTS.md` is intentionally minimal and points here for canonical project docs.
