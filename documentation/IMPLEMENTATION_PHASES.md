# Implementation Phases Timeline

This document captures the implementation and test checkpoints for the
Star6E standalone bring-up and high-FPS unlock work.

Source: extracted from historical session checkpoints (archived from prior `AGENTS.md` logs).

## Session Checkpoints
- 2026-02-21 checkpoint A:
  - Added/expanded `snr_toggle_test` for faster iteration:
    - new dimensions: `setres` timing (`pre|post|both`), held VIF timing (`none|pre|post`), sticky state mode.
    - new filters: `--init-snr-dev`, `--setres-timing`, `--fps-timing`, `--hold-vif`.
    - new output mode: `--quiet` for large sweeps.
  - Added remote-safe workflow enhancements:
    - reboot-enforced cold-state testing (`--reboot-before-run`) used for all high-risk runs.
    - dmesg delta always captured after each run.
  - Implemented VIF workmode correction in standalone code:
    - `main.c` and `snr_toggle_test.c` now use `I6_VIF_WORK_1MULTIPLEX` (Maruko-like) instead of RGB realtime.
    - removed prior dmesg error `MI_VIF_IMPL_SetDevAttr ... workmode 3 ... not support`.
  - Cold-state result remains unchanged so far:
    - multiple rebooted sweeps (including 300 sticky-state scenarios) still gave `mode_ok=0, fps_ok=0`.
    - `MI_SNR_SetFps` continues failing with `-1608835041`.
    - active mode still stuck at index `0` in cold state.
  - Persistent clue:
    - kernel/dmesg still reports `Connect IMX415_HDR_init_driver SEF to vif sensor pad 0` on cold boot.

- Immediate next-step plan after this checkpoint:
  1. Add plane-mode stress toggles (`TRUE`, `FALSE`, `TRUE->FALSE`) with readback (`MI_SNR_GetPlaneMode`) in `snr_toggle_test`.
  2. Add a minimal single-sequence runner mode (one scenario, full call-by-call readback) to compare:
     - cold boot vs after a short `majestic` prewarm.
  3. Capture side-by-side logs for the exact first successful latch path after majestic and replay the same call order in standalone.

- 2026-02-21 checkpoint B:
  - Major finding from cold-state sweeps:
    - `plane=hdr` (`MI_SNR_SetPlaneMode(..., TRUE)`) makes sensor mode latch succeed on cold boot for mode index `3`.
    - with `plane=hdr` and requested `-f 120`, mode latches but fps set still fails; readback sticks around `24`.
    - with `plane=hdr` and requested `-f 24`, both mode and fps succeed consistently.
  - Contrasting behavior:
    - `plane=linear` and `plane=hdr-linear` both remain mode-latch failures in cold state.
    - experimental `plane=hdr-post-linear` also failed to recover `120fps`.
  - Interpretation:
    - cold boot is likely entering/behaving as an HDR sensor path (`...IMX415_HDR...SEF`) and linear mode path is not being correctly unlocked by current init order.
    - mode index numbering/availability may be plane/HDR-state dependent in practice.
  - Tooling updates for repeatability:
    - `snr_toggle_test` now supports `--plane-mode linear|hdr|hdr-linear|hdr-post-linear`.
    - quiet and filter options are now usable for targeted large sweeps.

- Immediate next-step plan after checkpoint B:
  1. Add explicit readbacks around plane/HDR state before and after `SetPlaneMode` + `SetRes`:
     - include `MI_SNR_GetPlaneMode`, `MI_SNR_GetCurRes`, and per-mode list snapshots.
  2. Reproduce the best known unlock condition in `venc` with a guarded debug option (no default behavior change):
     - candidate option: temporary HDR pre-latch sequence.
  3. Validate whether a controlled HDR->linear transition can retain high-fps mode latching, then reattempt `60/90/120`.

- 2026-02-21 checkpoint C:
  - Process hardening:
    - Added explicit checkpoint cadence rules in this file to ensure periodic persistence of progress and next-step plans during long sessions.
  - Current technical state summary:
    - standalone target remains `star6e-standalone/` only.
    - cold boot high-fps issue is still unresolved; best cold latch observed so far uses HDR plane mode but remains fps-limited (~24/31 observed).
    - `majestic` prewarm can still unlock mode latching paths not yet reproduced reliably in pure cold standalone init.
  - Immediate next-step plan after checkpoint C:
    1. Implement readback instrumentation for plane-state and active mode snapshots around every plane/res/fps call path in `snr_toggle_test`.
    2. Run a bounded cold-state matrix focused on HDR->linear transition sequences and capture `dmesg` deltas for each.
    3. Promote one successful harness sequence into `venc` behind an opt-in debug flag and verify cold boot behavior without `majestic`.
    4. Probe runtime `/proc` sensor controls while `venc`/`majestic` is active (for example `echo 'setfps 0 {fps}' > /proc/mi_modules/mi_sensor/mi_sensor0`) and record which writes actually change behavior.
    5. Map any effective `/proc` operations back to corresponding MI SDK APIs/call ordering, then reproduce them in standalone init flow.

- 2026-02-21 checkpoint D:
  - Tooling updates:
    - Added `--trace-state` to `snr_toggle_test` for call-boundary snapshots
      (`GetCurRes`, `GetFps`, `GetPadInfo`) and mode-table dumps.
    - Hardened reboot handling in `star6e-standalone/scripts/remote_test.sh`:
      now waits for SSH-down transition before waiting for reconnect.
  - Key experimental results:
    - Cold + `plane=linear`: mode table remains linear (4 modes), but `SetRes(mode 3)` still leaves active mode at index `0`.
    - Cold + `plane=hdr`: mode table remaps to 7 HDR modes; `SetRes(mode 3)` selects HDR mode index `3` (`2592x1944@30_HDR`), and `SetFps(120)` fails as expected.
    - Cold + `plane=hdr-linear`: table returns to linear modes, but active mode still sticks at index `0`.
    - Cold + `plane=hdr-post-linear` with post `SetRes`: active mode returns to linear index `0`; no high-fps unlock.
  - `/proc` validation:
    - `setmirrorflip` accepted but does not fix cold mode-latch.
    - `setfps` acceptance tracks current active mode constraints; no bypass path found for cold-stuck state.
  - Immediate next-step plan after checkpoint D:
    1. Add an explicit "HDR mode-index remap guard" in `venc`: refuse or remap linear-target mode indices when HDR plane mode is active, to avoid false-positive mode latches.
    2. Investigate `MI_SNR_CustFunction` commands from Maruko/IPC demos for explicit HDR/linear context switching before `SetRes`.
    3. Build a tiny two-phase probe in harness: dump mode table, switch context, dump again, then attempt `SetRes+SetFps` in the same context to isolate the exact context-switch boundary that Majestic performs.

- 2026-02-21 checkpoint E:
  - Developer snippet validation (cold state, exact order intent):
    - tested sequence equivalent to:
      `SetPlaneMode(linear) -> SetRes(1) -> SetFps(30) -> Enable`
    - command:
      `./star6e-standalone/scripts/remote_test.sh --reboot-before-run --timeout-sec 30 --run-bin snr_toggle_test -- --sensor-index 0 --sensor-mode 1 -f 30 --max-cases 1 --init-snr-dev 0 --setres-timing pre --fps-timing pre --hold-vif none --plane-mode linear --no-reset --quiet`
    - result:
      - `SetRes(1)` returned success but active mode still stayed `0` (`3840x2160@30`).
      - fps set to `30` succeeded, but mode latch did not.
  - Follow-up in same boot (to test "priming" effect):
    - command:
      `./star6e-standalone/scripts/remote_test.sh --timeout-sec 30 --run-bin snr_toggle_test -- --sensor-index 0 --sensor-mode 3 -f 120 --max-cases 1 --init-snr-dev 0 --setres-timing pre --fps-timing pre --hold-vif none --plane-mode linear --no-reset --quiet`
    - result:
      - still `mode_ok=0`, `fps_ok=0`; kernel reports `MI_SNR_IMPL_SetFps ... fail`.
  - dmesg deltas for these runs:
    - no new unlock signal; cold path still prints IMX415 HDR driver connect and SetFps failure when >30 is requested.
  - Conclusion:
    - this snippet alone is not sufficient to reproduce Majestic’s unlock behavior on cold boot.
  - Immediate next-step plan after checkpoint E:
    1. Add a tiny two-stage standalone probe that executes snippet stage-A then stage-B (`SetRes(target)+SetFps(target)`) in the same process/session to eliminate cross-process teardown effects.
    2. Trace `/dev/mi_vif` and `/dev/mi_isp` ioctls around Majestic startup and replay the missing preconditions before first `SetRes`.
    3. Keep current safe fallback behavior (30fps) unchanged while probing unlock preconditions.

- 2026-02-21 checkpoint F:
  - Added minimal unlock-focused harness:
    - `star6e-standalone/src/snr_sequence_probe.c`
    - build target `snr_sequence_probe` in `star6e-standalone/Makefile`
    - remote uploader updated to include probe binary:
      `star6e-standalone/scripts/remote_test.sh`
  - Probe design:
    - stage-1 (developer snippet focused):
      `SetPlaneMode(linear) -> GetRes(idx) -> SetRes(mode) -> SetFps(fps) -> Enable`
    - stage-2:
      `SetRes(target) -> SetFps(target)` in the same process/session.
  - Cold-state result with snippet-equivalent stage-1 (`mode=1,fps=30`) then target (`mode=3,fps=120`):
    - active mode stays index `0` after stage-1 and stage-2.
    - `SetFps(120)` fails with `-1608835041` (and milli-fps retry fails).
    - dmesg still shows IMX415 HDR connect and SetFps fail traces.
  - Validation that probe can observe unlocked path:
    - with `--prewarm-majestic`, same probe then reaches:
      - stage-2 `SetFps(120) -> 0`
      - active mode `idx=3` (`1472x816`), sensor fps raw around `31`.
  - Conclusion:
    - developer snippet sequence alone does not unlock cold boot on this board.
    - unlock precondition is still external to this sequence (likely additional SDK/VIF/ISP sensor setup side effects).
  - Immediate next-step plan after checkpoint F:
    1. Keep using `snr_sequence_probe` as primary harness (not matrix-first) for init-unlock work.
    2. Diff Majestic vs standalone pre-sensor-init ioctl activity on `/dev/mi_vif` and `/dev/mi_isp`, then replay only missing steps.
    3. Add one probe flag at a time for candidate preconditions (for example explicit VIF open/attr path) and retest in cold state with reboot gates.

- 2026-02-21 checkpoint E:
  - Trace instrumentation refinement:
    - Added `MI_SNR_GetPlaneMode` readback in `snr_toggle_test` snapshots.
    - Expanded snapshot mode-table dumps to include `after-plane-pre` and `after-setres-pre`.
  - Confirmed mode-table remap behavior:
    - `plane=hdr` changes `plane=1` and remaps mode table to 7 HDR-only entries (max 30fps).
    - In this state, requested mode index `3` resolves to `2592x1944@30fps_HDR`, not `1472x816@120`.
  - Confirmed linear-context behavior:
    - `plane=hdr-linear` returns to `plane=0` and restores the 4-entry linear table immediately.
    - Even with added timing slack (`--delay-ms 200`), `SetRes(mode 3)` still returns success but active mode remains `0`.
  - Maruko SDK scan result:
    - no obvious non-I2C `MI_SNR_CustFunction` command usage found in reference app paths.
    - common paths continue using `SetPlaneMode -> SetRes -> SetFps -> Enable`.
  - Immediate next-step plan after checkpoint E:
    1. Add a harness phase that applies `SetPlaneMode(hdr)` only long enough to query/remap tables, then reboots/reinits and retries linear mode set to test for persistent latch side effects.
    2. Add targeted probe for `MI_SNR_InitDev` with valid struct layout from Maruko headers (instead of ad-hoc struct) to rule out init-param mismatch.
    3. Cross-check runtime with a minimal VIF-only bring-up (without full VPE/VENC path) before `SetRes/SetFps` to see if sensor context requires an active capture graph on cold boot.

- 2026-02-21 checkpoint G:
  - Added explicit reverse-engineering step to workflow:
    - after current cold-state test matrix/probe runs, fetch and inspect `majestic` on host.
    - goal is to recover exact `start_sdk -> mi_snr_init` trigger order that unlocks high-FPS modes.
  - Added host helper script:
    - `star6e-standalone/tools/majestic_reverse.sh`
    - performs quick extraction (`file`, `readelf`, `strings`) and writes focused symbol/log hints under `/tmp/majestic_reverse`.
  - Immediate next-step plan after checkpoint G:
    1. Run cold-state probe batch first (no Majestic prewarm) and capture outcomes.
    2. Run `majestic_reverse.sh` and diff extracted init hints against standalone call order.
    3. Implement one missing prerequisite at a time in `snr_sequence_probe` and retest from cold boot.

- 2026-02-21 checkpoint H:
  - Automated cold-state batches (no prewarm):
    - `snr_sequence_probe` with baseline + `majestic-order` + `touch-vif`/`hold-vif`
      + `touch-venc` + `prime-isp` + `cust-pre` + combined variants all remained:
      `after-stage2 idx=0 (3840x2160)` and `MI_SNR_SetFps(120) -> -1608835041`.
  - Prewarm timing sweep (cold reboot before each run):
    - Majestic prewarm durations `0.2/0.4/0.6/0.8/1.0/2/3/4s` all unlocked probe stage-2:
      `MI_SNR_SetFps(120) -> 0`, `after-stage2 idx=3 (1472x816)`.
  - Mechanism checks:
    - short prewarm with standalone `venc` did **not** unlock.
    - short Majestic run followed by interrupt path did unlock when Majestic reached full startup.
  - Trace/diff updates:
    - refreshed host-side Majestic reverse report via `tools/majestic_reverse.sh`.
    - captured cold ioctl traces for:
      - Majestic short run,
      - probe full-combo,
      - standalone `venc` short run.
    - observed that simple sensor-order parity is insufficient; unlock correlates with broader
      Majestic startup side effects (module init sequence beyond current standalone pre-init path).
  - Immediate next-step plan after checkpoint H:
    1. Add targeted pre-init experiment to standalone probe/main that mirrors earliest Majestic module bring-up (sensor + vif + vpe + venc path) before requested mode/fps set.
    2. Re-run cold gated tests for `mode 2 @90` and `mode 3 @120` with the same summary extraction.
    3. Keep this timeline section updated after each batch (old/current/future plan + concrete command outcomes).

- 2026-02-21 checkpoint I:
  - Prioritized cold batch results:
    - strict rebooted sequences for developer-snippet order and Majestic-like order
      (with `cust-pre`, `query-count`, `set-orien`, `prime-isp-pre`, pre/post VIF/VENC touches,
      disable/enable timing variants) all still failed:
      `MI_SNR_SetFps(120) -> -1608835041`, active mode remained `idx=0`.
  - New high-value finding:
    - `MI_SNR_CustFunction` call-shape mismatch is confirmed in standalone path.
    - interposed call shows standalone passes:
      `pad=0 cmd=0x80046900 size=4 data=<ptr> dir=0`,
      but underlying `/dev/mi_sensor` ioctl (`0xc014690f`) from lib path does not preserve
      those fields in the expected slots (compared to Majestic trace).
  - ABI sweep (`--cust-abi 0..4`) outcome:
    - only ABI `0` is stable (returns success but still no unlock).
    - ABI `1/3/4` trigger kernel BUG in `MI_SNR_IMPL_CustFunction` (`Case !pDriverData BUG ON!!!`).
    - ABI `2` returns sensor error (`NULL pointer`, `-1608835066`).
  - Raw ioctl replay experiment:
    - Added `--cust-raw` with exact Majestic-like request layout for `0xc014690f`.
    - pre-ioctl trace confirms fields can be matched exactly:
      `a0=0x14, a1=0x80046900, a2=<payload ptr>, a3=-1, a4=4, a5=0`.
    - despite exact shape (including reuse of existing `/dev/mi_sensor` fd), cold run still triggers
      kernel BUG at `MI_SNR_IMPL_CustFunction`; therefore raw replay is not safe/usable as-is.
  - Tooling update for faster feedback:
    - added UDP log mirror wrapper:
      `star6e-standalone/scripts/remote_test_udp.sh`
    - forwards `remote_test.sh` output with timestamp + sequence to `udp://<host>:<port>`.
  - Immediate next-step plan after checkpoint I:
    1. Keep raw cust path disabled for unlock attempts (kernel-BUG risk); use API-level calls only.
    2. Build a Majestic-side `MI_SNR_CustFunction` interposer variant matrix to infer the true
       expected function ABI/packing at the boundary to `libmi_sensor`.
    3. After ABI is validated, re-run strict cold `mode 2@90` and `mode 3@120` reproducibility tests
       and verify both ioctl shape and resulting active mode/FPS.

- 2026-02-21 checkpoint J (context-window handoff):
  - Current status (high confidence):
    - cold boot path is still not unlocking high-FPS reliably in standalone:
      active mode often remains `idx=0` (`3840x2160@30`) and `SetFps(60/90/120)` fails with `-1608835041`.
    - one short Majestic prewarm still proves hardware path is capable:
      standalone can then reach requested high-fps mode indices (`2/3`) and `SetFps(...)` success.
    - `MI_ISP_*CmdLoadBinFile` is timing-sensitive:
      pre-VENC load can fail with bogus sdk version numbers, while late/post-VENC retry often succeeds.
  - What Majestic reverse/decompile helped with:
    - gave useful structure-level clues (`mi_sensor.ko` + `libmi_sensor.so`) around
      `MI_SNR_IOCTL_CustFunction` / `MI_SNR_IMPL_CustFunction` expectations.
    - confirmed our earlier assumptions on simple call order are insufficient.
  - What Majestic reverse/decompile has not solved yet:
    - did not yet produce a single deterministic standalone init sequence that unlocks high-fps from strict cold boot.
    - cust/raw ioctl shape improvements (including `--cust-raw2`) reduced crashes in isolated cases,
      but still did not unlock high-fps on cold path.
  - Most valuable next steps (priority order):
    1. Complete strict differential ioctl timeline capture for first startup phase:
       compare Majestic vs `snr_sequence_probe` on `/dev/mi_sensor`, `/dev/mi_vif`, `/dev/mi_vpe`,
       with exact ordering and timing before first successful `SetFps`.
    2. Implement one explicit probe mode that replays the earliest Majestic-like module prelude
       (sensor + vif/vpe/venc minimal bring-up, keep VIF alive through stage-2), then retest cold.
    3. Keep raw-cust experiments gated/off by default (kernel BUG risk), and use API-level probe path
       for the main unlock search until a safe ABI/payload mapping is fully verified.
    4. Once a cold-boot unlock sequence is reproducible in probe, port exactly that sequence into
       standalone `venc` behind a debug flag first, then promote to default after repeated cold reboots.
    5. Continue checkpointing after each batch with:
       commands run, cold/warm state, active mode readbacks, `SetFps` result, and dmesg delta.

- 2026-02-21 checkpoint K:
  - Tooling/implementation updates:
    - `star6e-standalone/scripts/remote_test.sh` now supports `--ioctl-trace`
      (auto-copies `tools/ioctl_trace_preload.so` and exports `LD_PRELOAD` remotely).
    - `snr_sequence_probe` gained new prelude controls:
      - VPE: `--touch-vpe-pre/post`, `--hold-vpe-pre/post`
      - MJPEG VENC (Majestic-like ch2 touch): `--touch-venc-jpeg-pre/post`
      - timing: `--sleep-before-stage2-ms`
  - Differential trace result (high value):
    - Cold probe sequence can now mirror Majestic’s early ioctl order very closely, including:
      `mi_sensor c014690f (cust)` + early `mi_vif/mi_vpe` + `mi_venc` touches.
    - Despite near-identical ioctl order, strict cold runs still fail:
      active mode remains `idx=0`, `SetFps(120) -> -1608835041`.
  - Key proof of hidden precondition/state:
    - Running Majestic briefly (prewarm) then running the same mirrored probe sequence yields:
      `SetFps(120) -> 0`, active mode reaches `idx=3`.
    - Same probe sequence without Majestic prewarm fails.
    - Therefore, call-order parity alone is not sufficient; Majestic establishes additional state.
  - State-read comparison (cold vs prewarm):
    - Cold mirrored probe start:
      `idx=0 3840x2160 @30`, stage1 stays idx0, stage2 fps120 fails.
    - Prewarm mirrored probe start:
      `idx=2 1920x1080 @31`, stage1 transitions to idx1@30, stage2 fps120 succeeds to idx3.
    - This indicates prewarm changes initial sensor runtime state before probe stage1 begins.
  - Additional falsified hypotheses:
    - Added delays before stage2 (`0..2500ms`) did not unlock cold behavior.
    - Standalone `venc` short prewarm (without Majestic) did not unlock; follow-up probe still starts at idx0 and fails.
    - Probe self-prewarm (stage1-only run then full run without reboot) did not unlock.
  - `/proc` snapshot note:
    - After Majestic prewarm, `/proc/mi_modules/mi_sensor/mi_sensor0` reports current mode
      aligned with high-fps-capable path (e.g. current `1920x1080@90` view), while cold baseline
      does not expose that initialized state before any bring-up.
  - Immediate next-step plan after checkpoint K:
    1. Identify which Majestic startup side effect sets initial sensor state away from idx0:
       focus on module interactions not yet reproduced (likely beyond pure sensor/vif/vpe/venc ordering).
    2. Add one targeted “stage0 prelude” experiment in probe:
       perform full graph bring-up + bind/unbind cycle (VIF<->VPE<->VENC), teardown, then stage1/stage2.
    3. Inspect Majestic trace around the first successful-state transition boundary (just before/after
       first `MhalCameraOpen` / `AeInit`) and map missing ioctls/APIs into probe in minimal increments.
    4. Keep raw-cust path disabled in regular sweeps due BUG risk; continue strict reboot-gated validation.

- 2026-02-21 checkpoint L:
  - New high-confidence result:
    - same mirrored probe sequence can be both failing and succeeding depending on pre-state:
      - cold boot: fails (`idx0 -> SetFps(120) fail`)
      - cold + short Majestic prewarm: succeeds (`SetFps(120) -> 0`, reaches `idx3`)
    - this confirms a hidden runtime state transition, not just missing call ordering.
  - State-read proof:
    - cold run starts at `idx0 3840x2160@30`.
    - prewarmed run starts at `idx2 1920x1080@31` before stage1.
    - successful path then transitions stage1 `idx1@30`, stage2 `idx3` with fps set success.
  - Additional experiments and outcomes:
    - Stage2 timing sweep (`--sleep-before-stage2-ms 0..2500`): no unlock effect.
    - Probe self-prewarm (stage1-only run then full run without reboot): no unlock.
    - Standalone `venc` short prewarm then probe: no unlock (still starts idx0).
    - Added probe graph warmup (`--graph-cycle-pre` + bind/unbind cycle): no unlock.
      - observed warnings in dmesg:
        `MI_VIF_IMPL_CheckBindModeSupport ... not support different`.
  - Interpretation:
    - Majestic appears to establish sensor runtime context that probe/main standalone does not,
      even when probe replays near-identical early ioctl order.
    - Remaining gap likely sits in module-level bring-up side effects around camera-open/3A path
      rather than simple `SetRes/SetFps` ordering.
  - Immediate next-step plan after checkpoint L:
    1. Focus on reproducing the pre-state transition itself (`start idx2` instead of `idx0`) as the primary milestone.
    2. Instrument and compare first sensor state snapshot right after each candidate prelude step
       to find the exact call that moves `idx0 -> idx2`.

- 2026-02-22 checkpoint M (Maruko board integration):
  - Implemented dual SoC build split:
    - `SOC_BUILD=star6e` and `SOC_BUILD=maruko` in both root and standalone Makefiles.
    - each build now compiles only its matching backend source set.
  - Added Maruko ABI compatibility wrappers in `include/star6e.h`:
    - fixed `MI_SYS_*` SoC-id calling convention,
    - fixed `MI_VIF_*OutputPort*` symbol mapping,
    - fixed `MI_VENC_*` device+channel calling convention.
  - Added Maruko runtime compatibility shim:
    - `tools/maruko_uclibc_shim.c` (for missing uClibc symbol names on musl targets).
    - `remote_test.sh` now auto-builds/stages/preloads this shim on Maruko runs.
  - Upgraded remote test orchestration:
    - auto-detects target family from `ipcinfo`/device-tree,
    - auto-selects `SOC_BUILD` accordingly,
    - stages Maruko runtime libs for `infinity6c`.
  - Hardware-validated outcomes on `openipc-ssc378qe`:
    - PASS: `venc --list-sensor-modes --sensor-index 0` completes via `remote_test.sh`.
    - mode list matches expected IMX415 table (30/60/90/120 fps caps by mode).
    - FAIL (current blocker): stream path stops at
      `MI_VIF_SetDevAttr failed -1610211320` after successful `MI_SYS_Init` and sensor-mode selection.
  - Immediate next-step plan after checkpoint M:
    1. Validate Maruko `MI_VIF_DevAttr_t` layout and enum constants against Maruko headers; adjust compat structs/mappings.
    2. Re-run staged smoke tests after each VIF fix:
       list modes -> mode select -> VIF enable -> VPE enable -> VENC start.
    3. Keep Star6E parity checks in the loop after every compat-layer change.
    3. Investigate/align bind workmode compatibility in probe graph cycle
       (resolve `MI_VIF_IMPL_CheckBindModeSupport` warning) before retrying graph-based unlock attempts.

- 2026-02-21 checkpoint M (CUS3A phase complete):
  - Code/tool updates in this phase:
    - `snr_sequence_probe`:
      - added `--cus3a-seq-pre` (runs `MI_ISP_CUS3A_Enable` sequence `100 -> 110 -> 111`)
      - added `--cus3a-off-post` (runs `MI_ISP_CUS3A_Enable(000)` after post-stage1 prelude)
      - added graph prelude frame-pull option `--graph-cycle-pull-frames N`
    - maintained prior additions (`--graph-cycle-pre`, `--graph-cycle-ms`, `--sleep-before-stage2-ms`).
  - Completed cold test phase matrix:
    1. baseline mirrored sequence
    2. `--cus3a-seq-pre`
    3. `--cus3a-seq-pre --cus3a-off-post`
    4. `--cus3a-seq-pre --cus3a-off-post --graph-cycle-pre --graph-cycle-pull-frames 5`
  - Results:
    - all cases remained cold-stuck:
      - start/after-stage1/after-stage2 stayed `idx=0 3840x2160@30`
      - `MI_SNR_SetFps(120)` (and milli retry) failed with `-1608835041`
    - CUS3A toggles executed successfully (`100/110/111` and `000` all returned `0`) but did not unlock high-fps.
    - graph-cycle with frame pulling also did not unlock.
  - Conclusion after this phase:
    - CUS3A enable/disable ordering alone is not the missing unlock condition.
    - hidden pre-state established by Majestic remains unresolved.
  - Next-step priority (for next session):
    1. isolate the exact transition from cold `idx0` to prewarmed `idx2` using finer-grained snapshots around startup side effects.
    2. investigate Majestic’s additional module interactions around first `MhalCameraOpen/AeInit` beyond current standalone replication.

- 2026-02-22 checkpoint N (blocked run):
  - Attempted next-step strict ioctl value diff (cold `majestic` vs cold mirrored `snr_sequence_probe`)
    with safety timeouts and reboot gating.
  - First reboot + Majestic phase completed (Majestic exited under timeout wrapper), but the second
    reboot did not return within watchdog window; board became unreachable over SSH.
  - Status:
    - testing is currently blocked pending manual power-cycle/recovery.
  - Resume plan once board is back:
    1. rerun ioctl value diff script for first 40 `/dev/mi_sensor` `ioctl-pre` entries (`req`, `a1`, `a4`, `a5`).
    2. compare Majestic/probe value-level deltas for key requests (`0x4008690a`, `0x4008690b`, `0x40086905`, `0x40046900`, `0xc0386902`, `0xc0506903`).

- 2026-02-22 checkpoint O (driver-gated unlock found):
  - Alternate approach executed:
    - pulled and inspected target sensor driver module:
      `/lib/modules/4.9.84/sigmastar/sensor_imx415_mipi.ko`.
    - disassembly finding:
      - `pCus_SetVideoRes` checks internal field at offset `0x430` and only honors high-FPS mode switching when value is `0x80`.
      - otherwise it falls back to mode index `0` (`3840x2160@30`) path.
    - disassembly finding:
      - `pCus_sensor_CustDefineFunction` (`cmd_id == 0x23`) writes that same `0x430` field using payload shaped as two `u16` values (`reg`, `data`), with special handling for reg `0x300a`.
  - Cold-state probe validation (reboot-gated, no Majestic prewarm):
    1. `cmd=0x23`, payload `{0x300a, 0x80}` before `SetRes/SetFps`:
       - reproducible success:
         - mode `3` + fps `120` succeeds (`after-stage1 idx=3`, `fps=120`).
         - mode `2` + fps `90` succeeds (`after-stage2 idx=2`, `fps=90`).
    2. negative control `cmd=0x23`, payload `{0x300a, 0x40}`:
       - reproduces cold lock:
         - remains `idx=0 3840x2160@30`,
         - `MI_SNR_SetFps(120)` fails `-1608835041`.
  - Conclusion:
    - this is now a concrete and reproducible unlock mechanism in pure cold standalone flow,
      without running Majestic.
    - Majestic prewarm effect is likely achieved through the same/similar sensor custom latch path.
  - Immediate next-step plan:
    1. port this pre-latch into `star6e-standalone/src/main.c` as an opt-in debug flag first
       (`MI_SNR_CustFunction(cmd=0x23, {0x300a,0x80})` before `SetRes/SetFps`).
    2. verify end-to-end with `venc` cold boot at:
       - mode `2 @90`
       - mode `3 @120`
       and confirm stable video output.
    3. if stable across repeated cold reboots, make this path default for IMX415/Star6E and keep
       fallback safeguards.

- 2026-02-22 checkpoint P (implemented in standalone venc):
  - Documentation:
    - added dedicated reproduction/fix guide:
      `documentation/SENSOR_UNLOCK_IMX415_IMX335.md`
    - includes:
      - root-cause summary from driver analysis,
      - exact init sequence,
      - payload/command format,
      - cold-state test commands and expected results,
      - troubleshooting notes.
  - Standalone `venc` implementation:
    - integrated sensor pre-latch step in `star6e-standalone/src/main.c`:
      - `MI_SNR_CustFunction(cmd=0x23, payload{0x300a,0x80}, dir=driver)`
      - executed before `MI_SNR_SetRes` and re-applied in FPS retry path.
    - added runtime tuning flags:
      - `--sensor-unlock-off`
      - `--sensor-unlock-cmd`
      - `--sensor-unlock-reg`
      - `--sensor-unlock-value`
      - `--sensor-unlock-dir`
    - updated CLI help in `star6e-standalone/src/shared.c`.
    - added missing sensor custom ABI declarations in `star6e-standalone/include/star6e.h`.
  - Validation:
    - cold boot, standalone `venc` run with mode `3` and `-f 120`:
      - unlock log printed,
      - sensor selected as mode `3`,
      - pipeline starts at `1472x816 @ 120`,
      - stream stable and frame logs continue under timeout run.
  - Immediate next-step plan:
    1. run one more cold validation with mode `2 @90` through `venc` path.
    2. verify `--sensor-unlock-off` reproduces old 30fps lock (safety regression check).
    3. if both pass, keep defaults and reduce debug verbosity if needed.

- 2026-02-22 checkpoint Q (repository cleanup to standalone-only):
  - Removed legacy tracked implementations from repo history head:
    - `venc/`, `vdec/`, `osd/`, `sample/`, `deploy/`
    - legacy SDK trees: `sdk/gk7205v300`, `sdk/hi3516ev300`, `sdk/hi3536dv100`
  - Promoted standalone runtime libs to owned location:
    - `star6e-standalone/libs/star6e/*.so`
    - `star6e-standalone/Makefile` now links against standalone-owned libs path.
  - Updated top-level repo entrypoints to standalone-only:
    - `build.sh` now supports only `star6e|venc-star6e|clean`.
    - `manual_build.sh` now delegates to `star6e-standalone/manual_build.sh`.
    - `README.md` rewritten for standalone workflow only.
    - `.github/workflows/main.yml` now builds only standalone binaries.
    - `.gitignore` now tracks standalone outputs/binaries instead of legacy targets.
  - Immediate next-step plan:
    1. Continue sensor unlock/high-FPS work only in `star6e-standalone/src/` and its probes.
    2. Keep remote validation through `star6e-standalone/scripts/remote_test.sh` with reboot-gated cold runs.
    3. Avoid reintroducing legacy multi-platform code paths in this repository.

- 2026-02-22 checkpoint R (refactor review + cleanup pass):
  - Performed standalone-only refactor review and fixed residual quality issues:
    - removed implicit-declaration build warnings by extending `star6e-standalone/include/star6e.h`
      with missing SNR declarations (`InitDev`, `DeInitDev`, `GetCurRes`, `GetFps`,
      `GetPlaneMode`, `SetOrien`) and shared init param type.
    - de-duplicated local init-param probe structs in:
      - `star6e-standalone/src/snr_toggle_test.c`
      - `star6e-standalone/src/snr_sequence_probe.c`
    - normalized standalone runtime examples in `star6e-standalone/README.md`
      to `/tmp/waybeam_venc_test` paths.
    - hardened `--ioctl-trace` flow in `star6e-standalone/scripts/remote_test.sh`:
      - if preload `.so` is missing, script now auto-builds
        `tools/ioctl_trace_preload.so` with the Star6E cross compiler.
    - removed accidental binary artifact from tracking intent by ignoring:
      - `star6e-standalone/tools/ioctl_trace_preload.so` in `.gitignore`.
  - Validation:
    - `./manual_build.sh` now completes with no implicit-declaration warnings.
    - `bash build.sh star6e` succeeds for all standalone binaries.
  - Immediate next-step plan:
    1. keep this refactor-only PR focused (standalone cleanup + warning cleanup), no behavior changes.
    2. continue sensor unlock/high-FPS experiments only after this cleanup lands.

- 2026-02-22 checkpoint S (Makefile + docs consolidation):
  - Build entrypoint migration:
    - added root `Makefile` with targets `build`, `stage`, `clean`, `toolchain`, `remote-test`.
    - removed redundant wrappers `build.sh`, `manual_build.sh`, and `star6e-standalone/manual_build.sh`.
    - CI workflow switched to `make build`.
  - Documentation consolidation:
    - added canonical `documentation/` folder and moved/centralized sensor unlock + timeline docs.
    - legacy standalone unlock doc path now points to canonical docs.

- 2026-02-22 checkpoint T (dual-backend split kickoff):
  - Backend split scaffold started in `star6e-standalone`:
    - added backend abstraction header:
      - `star6e-standalone/include/backend.h`
    - added runtime backend detection module:
      - `star6e-standalone/src/backend_detect.c`
    - added Maruko backend stub entrypoint:
      - `star6e-standalone/src/backend_maruko.c`
    - integrated backend selection into main flow:
      - `--soc auto|star6e|maruko`
      - auto-detect via MI runtime symbol probing
      - Maruko currently exits with explicit "not implemented" message
    - updated build wiring:
      - `star6e-standalone/Makefile` now compiles backend modules into `venc`
  - Validation:
    - clean smoke build executed from repo root:
      - `make clean && make build`
    - all standalone binaries compile successfully:
      - `venc`, `snr_toggle_test`, `snr_sequence_probe`
  - Documentation updates:
    - added architecture/rollout plan:
      - `documentation/DUAL_BACKEND_SPLIT_PLAN.md`
    - updated docs index and current status with dual-backend kickoff state.
    - recorded upcoming Maruko toolchain archive:
      - `toolchain.sigmastar-infinity6c.tgz` (same `TOOLCHAIN_URL`)
  - Immediate next-step plan:
    1. Extract Star6E pipeline path into a dedicated backend module with no behavior change.
    2. Re-run Star6E runtime smoke tests (cold and normal) to confirm parity after extraction.
    3. Only then start Maruko backend implementation and SSC377 smoke testing.

- 2026-02-22 checkpoint U (backend dispatcher + Star6E extraction):
  - Completed no-behavior-change extraction of Star6E runtime path:
    - moved Star6E implementation entrypoint into:
      - `star6e-standalone/src/backend_star6e.c`
      - exported as `star6e_backend_entrypoint(argc, argv)`
    - introduced thin top-level dispatcher:
      - `star6e-standalone/src/main.c`
      - parses `--soc`, runs backend detection once, dispatches to selected backend.
  - Compatibility handling:
    - `--soc` is still accepted by Star6E parser path for passthrough compatibility.
    - runtime selection output remains visible before backend startup.
  - Build wiring:
    - `star6e-standalone/Makefile` now links:
      - `src/main.c src/backend_star6e.c src/shared.c src/backend_detect.c src/backend_maruko.c`
  - Validation:
    - clean rebuild succeeded:
      - `make clean && make build`
    - all standalone binaries compile successfully.
    - remote parity smoke tests executed:
      - `--soc star6e`: stream path runs and frame output is stable.
      - `--soc maruko`: exits cleanly with explicit stub message.
      - initial `--soc auto` with runtime symbol probing caused intermittent startup
        failure (`MI_VPE_CreateChannel`), traced to unsafe pre-init probing side effects.
  - Follow-up hardening:
    - changed auto policy to safe Star6E default routing (no pre-init `dlopen/dlsym` probing).
    - re-tested `--soc auto` remotely; stream path now matches explicit Star6E behavior.
  - Immediate next-step plan:
    1. Execute on-device parity smoke tests for Star6E (`--soc auto` and `--soc star6e`).
    2. Confirm no regression in sensor unlock behavior and stable Ctrl+C shutdown path.
    3. Keep Maruko backend as explicit stub until Star6E parity is confirmed.

- 2026-02-22 checkpoint V (auto-detect via ipcinfo family):
  - Replaced temporary auto safe-route logic with runtime family detection:
    - `detect_soc_backend()` now runs:
      - `/usr/bin/ipcinfo --family` (fallback `ipcinfo --family`)
    - maps family values to backend:
      - `infinity6e`/`star6e`/`ssc338` -> Star6E backend
      - `infinity6c`/`maruko`/`ssc377` -> Maruko backend
    - unknown or unavailable family falls back to Star6E with explicit reason string.
  - Why:
    - avoids unstable pre-init MI library probing while still providing meaningful auto selection.
  - Validation:
    - target reports `ipcinfo --family` as `infinity6e`.
    - `venc --soc auto` now selects Star6E with reason:
      `ipcinfo --family=infinity6e`.
  - Immediate next-step plan:
    1. Keep this as auto-detect baseline for Star6E + future Maruko smoke tests.
    2. When SSC377 is connected, verify `--soc auto` resolves to Maruko before backend bring-up.

- 2026-02-22 checkpoint W (auto-detect via device-tree first):
  - Investigated `ipcinfo --family` source on target:
    - `ipcinfo` binary references `/proc/device-tree/compatible` and model nodes.
    - target nodes confirmed:
      - `/proc/device-tree/compatible = sstar,infinity6e`
      - `/proc/device-tree/model = INFINITY6E SSC012B-S01A`
  - Detection refinement:
    - `detect_soc_backend()` now reads family hints directly from:
      1. `/proc/device-tree/compatible`
      2. `/sys/firmware/devicetree/base/compatible`
      3. `/proc/device-tree/model`
      4. `/sys/firmware/devicetree/base/model`
    - uses `ipcinfo --family` only as fallback if direct nodes are unavailable.
  - Result:
    - avoids dependence on external helper binary for normal path.
    - keeps robust fallback behavior for minimal images.

- 2026-02-22 checkpoint X (Maruko skeleton lifecycle + toolchain target):
  - Implemented Maruko backend lifecycle skeleton in:
    - `star6e-standalone/src/backend_maruko.c`
  - New behavior when `--soc maruko` is selected:
    - runs staged flow with explicit logs:
      1. init (placeholder)
      2. configure graph (placeholder)
      3. run (explicit "pending" error)
      4. teardown (placeholder cleanup)
    - exits cleanly with non-zero status until stream path is implemented.
  - Updated backend interface to pass argv into Maruko entrypoint:
    - `maruko_backend_entrypoint(const SocSelection*, int argc, const char* argv[])`
    - top-level dispatcher forwards arguments accordingly.
  - Added Maruko toolchain convenience target:
    - root `Makefile`: `make toolchain-maruko`
    - fetches `toolchain.sigmastar-infinity6c.tgz` from existing `TOOLCHAIN_URL`.
  - Immediate next-step plan:
    1. Add Maruko config parsing structure in skeleton path (shared with Star6E where possible).
    2. Implement minimal Maruko `MI_SYS` + sensor query + graceful teardown.
    3. Validate on SSC377 hardware with `--soc auto` and `--soc maruko`.

- 2026-02-22 checkpoint Y (minimal real Maruko bring-up):
  - Extended Maruko skeleton to perform real minimal backend operations:
    - parse Maruko-relevant args:
      - `--sensor-index`
      - `--list-sensor-modes`
    - `stage init`:
      - `MI_SYS_Init`
    - `stage configure`:
      - sensor probe via `MI_SNR_QueryResCount` + `MI_SNR_GetRes`
    - `stage run`:
      - supports `--list-sensor-modes` output path
      - non-list run still exits with explicit "stream pending" message
    - `stage teardown`:
      - `MI_SYS_Exit`
  - Remote validation on SSC338Q target:
    - `--soc maruko --list-sensor-modes --sensor-index 0`:
      - success, mode list printed, clean exit.
    - `--soc maruko` with stream args:
      - expected clean non-zero exit with pending-stream message.
    - `--soc auto`:
      - Star6E stream path still works unchanged.
  - Immediate next-step plan:
    1. Introduce Maruko backend config model for stream options (resolution/fps/codec/transport).
    2. Implement first real Maruko stream path (minimal VIF/VPE/VENC chain) behind `--soc maruko`.
    3. Smoke-test on SSC377 hardware once connected.

- 2026-02-22 checkpoint Z (Maruko first stream path on Star6E testbed):
  - Implemented first Maruko stream-graph attempt in `backend_maruko.c`:
    - sensor select (`SetPlaneMode/SetRes/SetFps/Enable`)
    - VIF + VPE + VENC bring-up and bind
    - compact UDP send loop with bounded no-frame timeout + clean teardown
  - Star6E-board smoke results:
    - `--soc maruko --list-sensor-modes --sensor-index 0`: pass
    - `--soc maruko -m rtp ... --sensor-index 0 --sensor-mode 2`:
      - graph config succeeds, but no frame delivery
      - exits fast with `no encoder data` and kernel warnings around VIF/VPE workmode compatibility
    - `--soc auto` and `--soc star6e`: streaming path still works (no regression)
  - Hardware switch gate:
    - Maruko backend implementation is ready for first true SoC validation on SSC377.
    - Next meaningful step is testing this path on actual Maruko hardware and iterating there.

- 2026-02-22 checkpoint AA (Maruko VIF API alignment complete):
  - Refactored Maruko VIF ABI usage to match Maruko SDK expectations:
    - added Maruko-specific VIF type/prototype wrappers in `star6e-standalone/include/star6e.h`:
      - `MI_VIF_GroupAttr_t`
      - `MI_VIF_DevAttr_t` (input rect + pixel format layout)
      - `MI_VIF_OutputPortAttr_t`
      - `MI_VIF_CreateDevGroup` / `MI_VIF_DestroyDevGroup`
    - updated Maruko VIF bring-up in `star6e-standalone/src/backend_maruko.c`:
      - now uses `CreateDevGroup -> SetDevAttr -> EnableDev -> SetOutputPortAttr -> EnableOutputPort`
      - added explicit failure-path rollback (`DisableOutputPort`, `DisableDev`, `DestroyDevGroup`)
      - updated teardown to always destroy VIF group.
  - Smoke-test results on `root@192.168.1.10`:
    - `--list-sensor-modes` path: pass (unchanged).
    - stream path (`-m rtp ...`):
      - now progresses past prior `MI_VIF_SetDevAttr` failure.
      - current blocker moved to VPE stage:
        - `[vpe] failed to open /dev/mi/vpe!(Bad file descriptor)`
        - `[vpe] failed to ioctl 0x407c6900!(Bad file descriptor)`
  - Immediate next-step plan:
    1. Align Maruko VPE ABI/prototypes and verify correct device/symbol path for `libmi_vpe.so`.
    2. Re-run cold smoke test to confirm VIF stays stable and graph advances past VPE.
    3. Once VPE opens cleanly, validate end-to-end stream loop (`MI_VENC_Query/GetStream`) and packet output.

- 2026-02-22 checkpoint AB (Maruko graph switched from VPE to ISP/SCL):
  - Root-cause confirmation:
    - Maruko target lacks `/dev/mi/vpe` in runtime device nodes.
    - sample reference path in Maruko SDK/demo uses:
      `VIF -> ISP -> SCL -> VENC` (not VPE).
  - Implementation updates:
    - replaced Maruko middle-pipeline bring-up in `backend_maruko.c`:
      - removed VPE dependency in runtime path.
      - added local `dlopen/dlsym` loaders for:
        - `libmi_isp.so` (`CreateDevice/CreateChannel/SetChnParam/StartChannel/SetOutputPortParam/EnableOutputPort`)
        - `libmi_scl.so` (`CreateDevice/CreateChannel/SetChnParam/StartChannel/SetOutputPortParam/EnableOutputPort`)
      - bind chain now configured as:
        - `VIF -> ISP` (realtime + low-latency)
        - `ISP -> SCL` (realtime + low-latency)
        - `SCL -> VENC` (framebase)
      - teardown updated for new unbind order and ISP/SCL shutdown.
    - `remote_test.sh` now stages `libmi_scl.so` for Maruko runs.
  - Smoke-test outcome on `root@192.168.1.10`:
    - mode listing still passes.
    - stream run now advances past former VPE blocker and reaches AE/ISP startup.
    - new blocker moved forward to encoder creation:
      - `ERROR: [maruko] MI_VENC_CreateChn failed -1610473469`
  - Immediate next-step plan:
    1. Align Maruko VENC ABI/signature and channel attribute layout (`CreateChn/StartRecvPic/Query/GetStream`) against Maruko headers.
    2. Verify Maruko VENC device/channel id usage (device id handling differs from Star6E path).
    3. Re-run stream smoke and confirm transition from create failure to active packet output.

- 2026-02-22 checkpoint AC (Maruko VENC ABI aligned, stream loop live):
  - Root cause confirmed:
    - Maruko `MI_VENC_CreateChn` rejected Star6E-style RC enum values.
    - Specifically, H.265 CBR mode must use Maruko RC mode id `10`.
  - Implementation updates:
    - `backend_maruko.c` now uses Maruko-native VENC ABI types (`i6c_venc_*`) for:
      - create dev/channel
      - start/stop recv
      - query/get/release stream
    - added explicit VENC device lifecycle in Maruko backend:
      - `MI_VENC_CreateDev` before channel create
      - `MI_VENC_DestroyDev` during teardown (only when created by this process)
    - switched Maruko stream-loop stat/pack/stream structs to Maruko layout to avoid pack ABI mismatch.
    - bound RC mode values to Maruko-compatible constants:
      - H.264 CBR: `1`
      - H.265 CBR: `10`
  - Smoke-test outcome on `root@192.168.1.10`:
    - stream path now passes VENC creation and enters live send loop.
    - observed frame output (`frame 30/60/...` with non-zero bytes) for 35s timeout window.
    - dmesg delta clean (no new warnings/errors), board stayed responsive.
  - Immediate next-step plan:
    1. Validate H.264 CBR and additional resolution/fps combos on Maruko board.
    2. Wire proper RTP packetizer in Maruko path (currently compact fallback for `-m rtp`).
    3. Re-check teardown/startup robustness across multiple consecutive runs and reboots.

- 2026-02-22 checkpoint AD (Maruko driver limitation acknowledged; defer sensor-depth work):
  - Updated operational targets:
    - Maruko board host: `root@192.168.1.11`
    - stream destination host: `192.168.1.2`
    - keep passing `--isp-bin /etc/sensors/imx415.bin` in Maruko tests.
  - Current driver behavior:
    - direct in-process Maruko ISP-bin API load is unstable on this image and can abort stream bring-up.
    - for now, backend keeps argument compatibility and logs a deferred warning instead of forcing load.
  - Decision:
    - treat high-FPS/deeper sensor mapping and stable direct ISP-bin API load as **fix later** until the newer driver is installed.
    - near-term validation scope on Maruko is stable 30fps streaming and backend architecture correctness.
  - Immediate next-step plan:
    1. Verify stable 30fps stream on H.265 and H.264 to `192.168.1.2`.
    2. Keep `--isp-bin` in test commands for consistency, while documenting deferred direct-load behavior.
    3. Resume deeper sensor/fps mapping after driver replacement.

- 2026-02-22 checkpoint AE (Maruko stream path validated: compact + RTP):
  - Maruko backend stream path is now functionally working on `openipc-ssc378qe` (`infinity6c`):
    - H.265 compact transport: frame loop active with continuous packet output.
    - H.265 RTP transport (`-m rtp`): packetizer path active and receiver shows visible frames.
  - Key implementation changes behind this checkpoint:
    - Maruko RTP packetizer implemented in backend path (instead of compact fallback).
    - Maruko VENC source mode configured for ring DMA input.
    - private ring pools configured via `MI_SYS_ConfigPrivateMMAPool` for SCL/VENC path.
    - Maruko ISP channel sensor id mapping corrected to use pad index.
  - Validation notes:
    - mode listing remains runtime-queried (`MI_SNR_QueryResCount`/`MI_SNR_GetRes`), not hardcoded.
    - local dual-build smoke passes for both `SOC_BUILD=star6e` and `SOC_BUILD=maruko`.
  - Immediate next-step plan:
    1. Keep Maruko smoke matrix focused on stability (H.265 compact + H.265 RTP).
    2. Add/confirm H.264 behavior on same backend path.
    3. Defer Maruko deep sensor/high-FPS work until planned newer driver update.

- 2026-02-22 checkpoint AF (Maruko libs vendored in-repo):
  - Added bundled Maruko runtime/link libs under:
    - `star6e-standalone/libs/maruko/`
  - Updated build/test defaults to use local Maruko libs:
    - `star6e-standalone/Makefile` now defaults `MARUKO_MI_LIB_DIR`/`MARUKO_COMMON_LIB_DIR`
      to `star6e-standalone/libs/maruko`.
    - `star6e-standalone/scripts/remote_test.sh` now defaults to the same local lib folder.
  - Result:
    - deploying to target is now self-contained (compiled binary + bundled libs),
      without requiring external Maruko SDK lib paths on host.
  - Immediate next-step plan:
    1. Keep local dual-build smoke (`SOC_BUILD=star6e`, `SOC_BUILD=maruko`) as pre-PR gate.
    2. Continue Maruko runtime smoke tests using bundled lib set.

- 2026-02-22 checkpoint AG (targeted-build cleanup + defaults alignment):
  - Architecture cleanup:
    - removed runtime SoC detect/override path from `venc`.
    - deleted `star6e-standalone/src/backend_detect.c`.
    - top-level `main.c` now dispatches directly to the backend compiled by `SOC_BUILD`.
  - Runtime defaults aligned:
    - stream mode default is now `RTP`.
    - codec default is now `H.265 CBR`.
  - Documentation refresh:
    - updated active plan docs to reflect targeted-build-only architecture and current remaining work.
  - Remaining focus after this checkpoint:
    1. Maruko H.264 parity validation.
    2. Deferred Maruko sensor/fps depth work after driver refresh.
    3. Keep Star6E/Maruko smoke gates green after each merge.
