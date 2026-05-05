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
  5. Maruko runtime/link libs are vendored under `libs/maruko/`.
  6. Maruko shim is a normal runtime dependency (`libmaruko_uclibc_shim.so`), not a preload requirement.
  7. JSON config + HTTP API runtime is live again.
     - current Star6E `venc` validation uses `/etc/venc.json` and `scripts/star6e_direct_deploy.sh`.
     - historical rollback/postmortem remains in `documentation/JSON_CONFIG_ROLLBACK_NOTES.md`.

## Remaining Implementation Steps (Priority Order)
1. JSON config runtime hardening (top priority):
   - keep user-facing config simple and unambiguous,
   - harden schema validation behavior (strict vs compatible mode),
   - add explicit migration handling/checks for future schema versions,
   - validate Star6E output parity against the production `/etc/venc.json` path.
2. 3A control review (Star6E-first):
   - review/confirm default SigmaStar 3A activation state in standalone flow,
   - evaluate tunable AE/AWB cadence controls for high-fps CPU reduction (for example AE every 2nd/3rd frame),
   - document API hooks and backend differences before rollout,
   - implement on Star6E first and queue Maruko port as follow-up.
3. Versioning and release traceability:
   - enforce SemVer via `VERSION`,
   - require `HISTORY.md` update for each PR/iteration.
4. HTTP/API contract hardening:
   - keep endpoint/payload/status contract synchronized in `documentation/HTTP_API_CONTRACT.md`,
   - expand validation around live vs restart-required setting classes,
   - keep Maruko stubs explicit where behavior is not yet implemented.
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

The authoritative backlog now lives in
`documentation/MARUKO_PARITY_PLAN.md` (verified-against-source gap audit
plus phased plan).  This section keeps a short pointer + the
runtime-agnostic items that don't fit the parity plan.

- **Verified gaps and phase tracker:**
  `documentation/MARUKO_PARITY_PLAN.md`.  As of v0.9.11 the closed gaps
  are: aspect-ratio precrop (Phase 1), debug OSD overlay code +
  runtime (Phase 2 + 2b), IMU/BMI270 (closed — no hardware on
  192.168.2.12), cold-boot sensor unlock (already on Maruko), frame-lost
  overshoot threshold (shared, raised to 1.5× in v0.9.8).  Open items:
  live AR-change reinit (Phase 4), audio capture (Phase 5), SD card
  recording (Phase 6), dual-VENC probe (Phase 7), driver-gated sensor
  depth (Phase 8), opt-in CPU-saving 3A throttle (Phase 9 — code
  ready, separate PR).
- **JSON config parity** with Star6E remains a maintenance line item
  for new fields.  Pattern: schema add in `venc_config.[ch]` →
  bridge in `maruko_config.[ch]` → consumer in
  `maruko_pipeline.c` / `maruko_runtime.c`.
- **HTTP runtime API:** read-only endpoints first; explicit
  `not_implemented` for write paths not yet ported.  Use Star6E
  validation as the gate before porting any SigmaStar API-touching
  apply handler.

## Comprehensive Settings Scope (JSON + HTTP)
- Full setting review and API scope is maintained in:
  - `documentation/CONFIG_HTTP_API_ROADMAP.md`

## Validation Gates
1. Build gate:
   - `make build SOC_BUILD=star6e`
   - `make build SOC_BUILD=maruko`
2. Star6E gate (board: `192.168.1.13`):
   - direct deploy cycle starts pipeline without crashes.
   - with matching sensor/ISP tuning, HTTP API is reachable and RTP payload output is non-zero.
   - no new VIF/VPE dmesg regressions vs baseline.
3. Maruko gate (board: `192.168.2.12`):
   - H.265 compact stream stable.
   - H.265 RTP stream stable.

## Notes
- `documentation/IMPLEMENTATION_PHASES.md` remains the historical timeline.
- This file tracks active forward implementation from the current baseline.
