# Image Stabilization — On-Device Test Plan

Self-contained schedule for validating digital image stabilization (DIS) on
Star6E. Written so a fresh session (no prior chat context) can execute it.
Tick the boxes and fill **Result** as you go; commit updates so progress
survives across sessions.

- **Branch:** `claude/cherry-pick-image-stabilizing-UkZ37` (PR #122)
- **Feature:** opt-in DIS on the VPE pipeline. A thread drains VPE port0,
  runs `MI_IVE_Shift_Detector` on a centre Y patch, and `MI_SYS_BufBlitPa`-
  crops a shifted window into VENC ch0's input. AE meter follows the crop.
  IMU gyro samples flow into a ring as a fusion seam (optical-only today).
- **Key code:** `src/star6e_pipeline.c` (stab block + `bind_and_finalize`),
  `src/star6e_runtime.c` (OSD panel anchor), `src/debug_osd.c`.
- **Config fields (video0):** `stabCropPct` (0=off, 50..100=crop %),
  `stabRecenterSpeed` (recenter time constant in frames; 0=stick). Both
  `MUT_RESTART` — edit `/etc/waybeam.json` then restart, not live `set`.

## Status

| Date | Tester | Firmware/sensor | Overall | Notes |
|------|--------|-----------------|---------|-------|
| 2026-05-20 | Claude (Opus 4.7) | star6e v0.12.0 / imx335 (ssc338q, 2560x1920@60) | PASS w/ caveats | Feature works (T0-T4 pass, T5 N/A no IMU). Two caveats: (1) stab caps fps ~60→~40 at 2048x1536 despite 44% idle CPU (pipeline serialization); (2) restart/teardown is intermittently fragile — 2 of ~5 stab restarts (incl. one plain 80→0) wedged the SoC into a D-state hang → watchdog reboot (~80s), always recovering clean. Wedge is the longstanding venc teardown class, not new to this PR. dmesg `DEBUG_LOCKS_WARN_ON` is the unrelated 8812eu wifi driver. |
| 2026-05-20 | Claude (Opus 4.7) | star6e (HW-crop refactor) / imx335 | PASS | **HW-crop refactor** — both caveats above resolved. port0 now hardware-crops the stab window (`MI_VPE_SetPortMode`=enc + per-detect `MI_VPE_SetPortCrop(0,0)`) straight into a VENC bind (zero-copy, no per-frame BufBlitPa); a tiny port1 256×256 tap feeds the detector, disabled on stop via a pause/park quiesce handshake. Legacy blit path retained as automatic fallback if the BSP rejects port1. Validated: HW mode engages (port1 works on SSC338Q); 60 fps at 1536×864; pan + AE-crop follow; **T6 teardown 5/5 clean restarts** (uptime continuous, no wedge) vs the historic ~40% wedge. Return-to-center now decays the offset *vector* (float accumulator) → straight diagonal to center, no per-axis tail. `[vpe0_P0_MAIN]` DW in `ps`/load-avg is normal on this SoC in all modes — not a health signal; use CPU idle% + fps + tick advance. |

## Target (from AGENTS.md → Deployment Targets)

- **Host:** `root@192.168.1.13` — Star6E ssc338q / imx335, sensor-index 0
- **ISP bin:** `/etc/sensors/imx335_greg_fpvVII-gpt200.bin`
- **Stream destination:** `192.168.1.2`
- Only run if the host is reachable; if offline, mark NOT RUN and stop.

## Pre-flight

```bash
# From repo root on the build host
make verify                      # both backends build, binaries OK
ssh root@192.168.1.13 'echo ok'  # confirm device reachable
```

If `make verify` fails, fix before deploying. If the device is unreachable,
do not proceed.

## Build + deploy

Preferred path uses the production `/etc/waybeam.json` + HTTP API:

```bash
make build SOC_BUILD=star6e
ssh root@192.168.1.13 "killall waybeam; sleep 2"
scp -O out/star6e/waybeam root@192.168.1.13:/usr/bin/waybeam
```

Always edit config with `json_cli` (never `sed`):

```bash
ssh root@192.168.1.13 "json_cli -s .video0.stabCropPct 80 -i /etc/waybeam.json"
ssh root@192.168.1.13 "json_cli -s .video0.stabRecenterSpeed 60 -i /etc/waybeam.json"
ssh root@192.168.1.13 "json_cli -s .system.verbose true -i /etc/waybeam.json"
```

Start as a daemon and capture logs:

```bash
ssh root@192.168.1.13 "nohup waybeam > /tmp/waybeam.log 2>&1 &"
sleep 10
```

Restore + stop when done with a case that changed config:

```bash
ssh root@192.168.1.13 "json_cli -s .video0.stabCropPct 0 -i /etc/waybeam.json"
ssh root@192.168.1.13 "json_cli -s .system.verbose false -i /etc/waybeam.json"
ssh root@192.168.1.13 "killall waybeam"
```

## Test cases

### T0 — Baseline, stab OFF (regression guard)
- [ ] `stabCropPct=0`, start, view the stream on `192.168.1.2`.
- [ ] Stream is full-frame; `zoomX/zoomY` still pan smoothly (0.11.0 ramp);
      `zoomPct` zoom still works.
- **Expect:** identical to pre-PR behavior. **Result:** PASS. stab absent/0 → full-frame 2560x1920 @ steady 60 fps. `set?video0.zoomPct=1.5` engages SCL (`Set SCL clock to 384 MHz`, crop/output reconfig); `zoomX/zoomY` accepted (`ok:true`), stream stays 60 fps after reset. Jitter/framing not visually checked (no display on bench; validated via API + logs).

### T1 — Stab ON, basic stabilization
- [ ] `stabCropPct=80`, `stabRecenterSpeed=60`, restart.
- [ ] Stream resolution shrinks to ~80% (smaller frame in SPS/PPS).
- [ ] Gently shake the camera: jitter is visibly reduced vs T0.
- [ ] `grep 'stab tick' /tmp/waybeam.log` → `meas`/`acc` values are sane
      (small, non-stuck), `max` matches the dead border.
- [ ] `grep 'stab: src=' /tmp/waybeam.log` → src/out dims look right.
- **Expect:** stabilized, cropped ch0; no crash. **Result:** PASS (with fps caveat). `stab: src=2560x1920 out=2048x1536 crop=80% recenter=60`; encoded + jpeg both shrink to 2048x1536. `stab tick` runs every 120 frames: `max=(256,192)` = exact dead border (half of 512x384), `pan=(500,500)` centered, `meas/acc` start (0,0) on a static bench and go non-zero+bounded once content moves (see T2). No `stab venc send failed` spam (watch-list clean). Expected `stab_crop_pct in use; ignoring zoom_pct` warning. **CAVEAT: fps drops 60→~40 with stab on, while CPU is ~44% idle — pipeline serialization (drain→Shift_Detector→BufBlitPa per frame), not CPU saturation. Not documented in the PR.** Camera couldn't be physically shaken, so visual jitter reduction not confirmed; detector loop + geometry confirmed correct.

### T2 — Live pan under stab
- [ ] With stab ON, pan via the API (alias maps to zoom_x/zoom_y):
      ```bash
      ssh root@192.168.1.13 "wget -q -O- 'http://127.0.0.1/api/v1/set?video0.zoomX=0.2'"
      ssh root@192.168.1.13 "wget -q -O- 'http://127.0.0.1/api/v1/set?video0.zoomY=0.8'"
      ```
- [ ] Framing recenters toward the requested point (direct, not ramped).
- [ ] `pan=(…)` in the stab tick log reflects the new center.
- **Expect:** crop window pans; stabilization still active. **Result:** PASS. `set?video0.zoomX=0.2` + `zoomY=0.8` → `pan=(500,500)` jumps directly to `pan=(200,800)` (not ramped). With content moving, `meas` non-zero and bounded (e.g. `meas=(-6,0) acc=(66,20)`, always within `max=(256,192)`) — confirms Shift_Detector + accumulator + dead-border clip all work. Stream stays ~42 fps.

### T3 — AE meter follows the crop
- [ ] With stab ON, frame so the crop covers a dark subject while the full
      sensor FOV includes bright sky.
- [ ] Pan the crop between bright and dark regions (T2 commands).
- [ ] Exposure tracks the *framed* (cropped) region, not the full sensor.
- [ ] `wget -q -O- http://127.0.0.1/api/v1/ae` before/after pan shows the
      exposure responding to the crop content.
- [ ] No `MI_ISP_CUS3A_SetAECropSize … failed` warnings in the log.
- **Expect:** AE meters on the crop, like the zoom path. **Result:** PASS. No `SetAECropSize … failed` warnings. `/api/v1/ae` exposes `active_precrop` (the AE metering window). Panning the crop to a different sensor region changed AE gain (`long_sensor_gain_x1024` 20571→17818; shutter pinned at min in the bright scene) — exposure tracks the framed/cropped region, not the full sensor. Scene lighting could not be staged on the bench, but the AE response to crop position confirms the wiring.

### T4 — Stab + dual recording mutual exclusion
- [ ] Set `record.enabled=true`, `record.mode="dual"`, keep `stabCropPct=80`,
      restart.
- [ ] `grep 'dual recording disabled' /tmp/waybeam.log` → warning present.
- [ ] ch0 main stream stays alive (no stall); dual ch1 is NOT started.
- **Expect:** warning logged, ch0 healthy. **Result:** PASS (with a behavior note). `record.mode=dual` + `stabCropPct=80` → warning fires: `dual recording disabled while image stabilization is active … cannot share VPE port0`. ch0 stays healthy (stab streaming 2048x1536, ~40 fps); no second VPE-port0 channel started. **NOTE: it does not fully disable recording — `[ts_recorder] started: …rec_…opus.ts` still fires and writes one .ts of the stabilized main stream (graceful downgrade dual→main-channel record). Confirm this is intended vs. a guard gap.** (The wedge that hit during this restart, see T6, was later reproduced on a plain stab toggle too, so it is not specific to dual.)

### T5 — IMU gyro ring plumbing (only if an IMU is wired)
- [ ] `json_cli -s .imu.enabled true`, set `i2cDevice`/`i2cAddr` for the
      board, restart with stab ON.
- [ ] `grep 'stab tick' /tmp/waybeam.log` → `gyro_n` > 0 and `gyro=(…)`
      shows non-zero rates when the camera moves.
- **Expect:** gyro data is frame-aligned and flowing (fusion is future
      work; values are diagnostic only). **Result:** N/A — no IMU wired (`imu.enabled=false`; bench has /dev/i2c-0..3 but no IMU on this unit). `stab tick` shows `gyro_n=0 gyro=(0,0,0)` as expected with IMU off. Not testable on this hardware.

### T6 — Teardown / restart stress
- [ ] Toggle `stabCropPct` 0→80→0 with a restart each time (3 cycles).
- [ ] `grep -iE 'fault|oops|panic|timeout|watchdog' ` over `dmesg` after each
      cycle → clean.
- [ ] Device stays responsive (SSH works) after each restart.
- **Expect:** no D-state hang, no kernel faults across restarts. **Result (HW-crop refactor):** PASS — 5/5 consecutive `S95waybeam restart` cycles clean (uptime continuous, no watchdog reboot). Root cause of the old wedge: the legacy single-port path made the stab thread the ONLY consumer of full-res VPE port0, and stopping it before unbinding VIF→VPE left the producer filling an un-drained queue → `[vpe0_P0_MAIN]` D-state. The HW-crop refactor binds port0→VENC (consumed by hardware, standard teardown) and only manually drains a tiny port1 tap, disabled via a quiesce handshake. **Prior result (legacy path, retained for history):** PARTIAL FAIL — intermittent teardown wedge. New-binary restart tally this session: T1 (off→80) clean; T4 (80→80+dual) WEDGE; cycle A (80+dual→0) clean; cycle B (0→80) clean; cycle C (80→0, plain toggle) WEDGE. Both wedges left `[vpe0_P0_MAIN]` in D-state, drove load avg up while CPU stayed mostly idle, made the SoC unreachable (~80s), then a watchdog/`panic=20` reset recovered to a healthy stab-correct boot every time. No venc/MI oops or hung-task survives the reset to inspect (reset wipes the ring). The only fault in post-boot dmesg is `DEBUG_LOCKS_WARN_ON(in_interrupt())` from the **8812eu wifi driver** (`_halmac_mutex_lock` ← `usb_recv_tasklet`) — unrelated to stab. Net: the wedge is the known venc teardown-fragility class (see [[venc_teardown_regression]] / [[venc_star6e_reinit_fragility]]), not introduced by this PR, but stab restarts do hit it. Device always recovered; never needed a physical power cycle.

## Watch-list (areas with no host-side coverage)

- `star6e_stab_reapply_vpe_port` uses `EnablePort` without a prior
  `DisablePort` and sets output depth 4/8 — confirm port0 comes up at full
  src dim and the drain keeps up (no `stab venc send failed` spam).
- First-frame path and the 2-deep buffer hold during teardown — confirm no
  leak/stall on stop (T6).

## Failure handling

- **Binary error / stall:** read `/tmp/waybeam.log`; fix; rebuild; redeploy.
- **dmesg fault/panic/timeout:** log it in `documentation/CRASH_LOG.md`
  (exact command + circumstances + dmesg), reboot before the next case.
- **Device unresponsive (SSH dead):** stop. Log in `CRASH_LOG.md`. Do not
  retry SSH. Request a power cycle, then redeploy from scratch.
- Refer to AGENTS.md → "Deployment Test Recovery" for the full loop.
