# JSON Config Rollback Notes

## Summary
- A JSON-config runtime implementation was attempted and tested on Star6E/Maruko.
- The implementation was **rolled back** after Star6E regressions in the main stream path.
- Current runtime remains the previous CLI-based `venc` flow.

## Observed Regression (Star6E)
- Symptom:
  - `venc` stayed in `waiting for encoder data...` with no RTP payload output.
- dmesg signals seen during failed runs:
  - VIF/VPE sync and enqueue errors (for example `layout type ... not sync err`).
  - input-port timeout/rewind warnings around VPE path.
- Additional risk during attempt:
  - ISP bin load sequencing changes could deadlock startup when first-frame arrival was required before load.

## Why We Rolled Back
- Regression affected the primary standalone streaming path on Star6E.
- The JSON migration was bundled with runtime-path behavior changes, making it hard to isolate failure quickly.
- Stability of the existing CLI pipeline is higher priority than config-interface migration.

## Guardrails For Next Attempt
1. Keep JSON migration in a dedicated feature branch until Star6E parity is proven.
2. Do not change VIF/VPE/VENC graph behavior in the same phase as parser/config plumbing.
3. Add strict parity gate before merge:
   - same resolution/fps/codec command profile must produce frames on cold boot.
   - compare dmesg deltas against known-good CLI baseline.
4. Stage rollout:
   - parser + in-memory config mapping first,
   - runtime wiring second,
   - advanced validation behavior last.

## Current State
- JSON/HTTP documentation remains as roadmap/contract material.
- Runtime implementation for JSON is not active in current codebase.
