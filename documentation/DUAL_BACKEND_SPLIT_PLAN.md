# Dual Backend Split Plan (Star6E + Maruko)

## Objective
- Keep one repository with two stable backend builds:
  - `SOC_BUILD=star6e`
  - `SOC_BUILD=maruko`
- Keep runtime deployment simple (`/usr/bin/venc` + `/usr/lib/*.so`).
- Continue backend split with build-time selection (no runtime backend autodetect).

## Current Implementation State
- Completed:
  1. Backend split by build target is in place.
  2. Runtime SoC autodetect/override in `venc` has been removed.
  3. Star6E backend is stable with default `RTP + H.265 CBR`.
  4. Maruko backend streams in compact and RTP modes.
  5. Maruko runtime/link libs are vendored under `star6e-standalone/libs/maruko/`.
  6. Maruko shim is a normal runtime dependency (`libmaruko_uclibc_shim.so`), not a preload requirement.
  7. JSON runtime attempt was rolled back after Star6E stream regressions.
     - current runtime remains CLI-based `venc` path.
     - rollback/postmortem: `documentation/JSON_CONFIG_ROLLBACK_NOTES.md`

## Remaining Implementation Steps (Priority Order)
1. JSON config migration (top priority):
   - keep user-facing config simple and unambiguous (`capture_resolution` + `fps` auto-select sensor mode),
   - split parser/config mapping from graph/runtime behavior changes,
   - harden schema validation behavior (strict vs compatible mode),
   - add explicit migration handling/checks for future schema versions,
   - validate Star6E output parity against CLI baseline before merge.
2. 3A control review (Star6E-first):
   - review/confirm default SigmaStar 3A activation state in standalone flow,
   - evaluate tunable AE/AWB cadence controls for high-fps CPU reduction (for example AE every 2nd/3rd frame),
   - document API hooks and backend differences before rollout,
   - implement on Star6E first and queue Maruko port as follow-up.
3. Versioning and release traceability:
   - enforce SemVer via `VERSION`,
   - require `HISTORY.md` update for each PR/iteration.
4. HTTP control API for live runtime updates:
   - add a local HTTP endpoint to query and update runtime settings (bitrate, GOP, codec profile, stream target, image controls),
   - define and implement setting mutability classes (`live`, `requires_rebind`, `requires_pipeline_restart`),
   - include Maruko stubs where behavior is not yet implemented,
   - keep endpoint/payload/status contract synchronized in `documentation/HTTP_API_CONTRACT.md`.
5. Maruko codec parity validation:
   - verify and document `264cbr` behavior in current Maruko graph path.
6. Maruko sensor-depth follow-up (deferred until newer driver):
   - mode/fps mapping re-validation,
   - direct ISP-bin API load stability,
   - >30fps validation.
7. Release hardening:
   - keep deploy checklist current for `/usr/bin` + `/usr/lib`,
   - keep smoke-test commands synchronized across README/workflow docs.

## Feature Rollout Policy
- For any feature that touches SigmaStar API behavior and may differ between SoCs:
  - implement and validate on Star6E first,
  - track Maruko port work explicitly in plan/checklists,
  - then port to Maruko after Star6E behavior is stable.
- For shared/runtime-agnostic features (JSON config parser, HTTP endpoint framework):
  - implement on both backends in same phase,
  - allow Maruko to ship with explicit stubs until API parity is finished.

## Maruko Follow-Up Backlog (Must Track Explicitly)
- JSON config:
  - keep parser/validation in sync with Star6E.
- HTTP runtime API:
  - support read-only endpoints first,
  - return explicit `not_implemented` for write paths not yet ported.
- SigmaStar API-touching runtime changes:
  - per-setting apply handlers for bitrate/GOP/image controls after Star6E validation.
- Sensor-depth backlog (driver-dependent):
  - mode/fps re-validation,
  - direct ISP-bin load stability,
  - >30fps verification once newer driver is available.

## Comprehensive Settings Scope (JSON + HTTP)
- Full setting review and API scope is maintained in:
  - `documentation/CONFIG_HTTP_API_ROADMAP.md`

## Validation Gates
1. Build gate:
   - `make build SOC_BUILD=star6e`
   - `make build SOC_BUILD=maruko`
2. Star6E gate (board: `192.168.2.10`):
   - baseline CLI run starts pipeline without crashes.
   - with matching sensor/ISP tuning, RTP payload output is non-zero.
   - no new VIF/VPE dmesg regressions vs baseline.
3. Maruko gate (board: `192.168.1.11`):
   - H.265 compact stream stable.
   - H.265 RTP stream stable.

## Notes
- `documentation/IMPLEMENTATION_PHASES.md` remains the historical timeline.
- This file tracks active forward implementation from the current baseline.
