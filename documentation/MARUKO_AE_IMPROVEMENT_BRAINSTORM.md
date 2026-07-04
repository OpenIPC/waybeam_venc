# Maruko custom-AE (throttle) — why it shifts colour + steps, and how to fix it

Design brainstorm. The Maruko `isp.aeEngine=custom` path saves ~10 CPU points
(see `MARUKO_CPU_PROFILE.md`) but has two quality defects the smooth Star6E
Cus3A does not: (1) a visible **colour shift**, and (2) noticeable **AE
"steps"** when brightness changes. This documents the root cause and a
concrete improvement path that keeps the CPU win.

**Status:** P1 (IIR controller) + P4 (pin ISP gain) are **implemented** in
`src/maruko_cus3a.c` (first cut) and bench-smoke-tested — the loop converges,
holds without oscillation, and pins ISP gain at 1.0×; `make verify` +
1699 tests green. Still needs **real-scene field tuning** (ALPHA/target) and
**P2 (real BV / colour)**, which requires on-device calibration and is not yet
done. P3/P5/P6 are open.

## Why the two backends differ

`isp.aeEngine=custom` means opposite things on the two chips:

| | Star6E custom (`star6e_cus3a.c`) | Maruko throttle (`maruko_cus3a.c`) |
|---|---|---|
| Who converges AE | the **vendor AE** (SigmaStar's tuned algorithm) | a **hand-rolled bang-bang loop** at 15 Hz |
| Supervisory thread does | only reads stats + enforces `SetExposureLimit` caps — never sets the AE value | **drives the whole AE** via a 3-stage integer cascade |
| Scene metadata (BV, F#) fed to colour pipeline | real, from the vendor AE | **hardcoded constants** |

On Star6E, custom mode is *supervisory-only*: the vendor's tuned AE+AWB keep
running (in-kernel/HW-assisted) with real scene brightness and its own damping,
so it stays smooth. On Maruko the throttle **stubs the vendor AE** (that's the
CPU saving) and replaces it with our own loop — but that loop is only ~10% of a
real AE algorithm. Both defects follow from that.

> Nuance: the I6C SDK shows AE runs on the ARM on *both* chips (no firmware 3A
> on I6C — see `MARUKO_CPU_PROFILE.md`). The point here is not "firmware vs
> software" — it is "the vendor's *tuned, damped* AE vs our *crude* stub."

## Defect 1 — colour shift

Two compounding causes:

**1a (primary) — fake scene-brightness metadata pins the colour pipeline.**
Every tick the loop pushes constant photometric metadata regardless of the real
scene (`maruko_cus3a.c:507-512`):

```c
ae_result.u4BVx16384 = 16384;   // APEX brightness value — PINNED
ae_result.FNx10      = 28;       // aperture — PINNED to F2.8
```

The ISP's light-source / CCM (colour-correction matrix) / LSC (lens-shading)
tables are blended as a function of estimated scene lux (BV). With BV frozen at
one value, the ISP keeps applying the colour correction for a single fixed lux
level, so colours drift as real brightness changes. The vendor AE feeds a
*real* BV, so its colour stays anchored to actual lux. `MarukoAeInfo`
(`maruko_cus3a.c:33-46`) exposes no BV to read back, so the loop punted to a
constant.

**1b (secondary) — AWB chases the AE steps.** The stub leaves AWB native at
sensor rate (~90–100 Hz) while AE jumps at 15 Hz in ~20% steps. Each AE step
shifts whole-frame luminance ~0.26 EV for a frame; the fast AWB re-estimates
R/G/B gains on that transient → a colour wobble synchronised to every AE step.

## Defect 2 — AE stepping

The control law is a **quantised bang-bang integrator with no smoothing** —
the worst combination (`maruko_cus3a.c:261-286, 471-516`):

```c
#define AE_STEP_NUM 12   // every move is ×1.2 or ÷1.2  = ~0.26 EV PER TICK
#define AE_STEP_DEN 10
...
if (delta >  AE_DEAD_BAND)      cur_shutter = step_up(...);   // one 20% jump
else if (delta < -AE_DEAD_BAND) cur_shutter = step_dn(...);
```

Four defects: (1) a fixed **20% (0.26 EV) quantum per tick** — human threshold
is ~1–2%, so every move is individually visible; (2) **bang-bang, not
proportional** — step size ignores error magnitude, so a big change converges
as a visible staircase and a small change overshoots by a full quantum and
hunts; (3) **no IIR/damping/interpolation** — the sensor snaps to each new value
in one frame; (4) 15 Hz **holds each jump ~8 frames**. The `SetAeParam` API
itself takes shutter in µs and gain ×1024 (sub-1% resolution) — the coarseness
is entirely ours.

## Proposed fixes (all keep the CPU win — control math is µs/tick)

**P1 — replace the bang-bang cascade with a log-domain IIR (damped)
controller.** *(fixes stepping — the biggest win)*
`maruko_cus3a.c:261-286, 471-516`. Work in total-exposure (EV) space:
`ratio = (target_Y+k)/(avg_y+k)`; `desired = applied*ratio` (proportional —
big error → big move); then damp `applied += alpha*(desired-applied)` in log
space, `alpha≈0.15–0.25`. Split the continuous total across shutter → analog
gain smoothly. Removes the staircase and the overshoot/hunt; also kills the
AWB-chasing wobble (1b) because luminance now moves smoothly.

**P2 — feed a real BV / F-number instead of the pinned constants.**
*(fixes colour shift)* `maruko_cus3a.c:507-512`. Derive `u4BVx16384` from the
exposure the loop is applying (APEX: `BV ≈ log2(C·avg_y/(shutter_s·gain))`) and
set `FNx10` to the lens's true F-number. **Calibrate on HW:** capture native-mode
BV at 3–4 light levels and fit the encoding so throttle BV matches native at the
same scene — do not ship blind.

**P3 — raise default `isp.aeFps` 15 → 30.** `venc_config.c:97`. Polish once P1
is smooth; the only lever with a (small) CPU cost — doubles the 46 KB stats read
+ grid sum per second, still tiny vs the native-AE cost removed. Optionally
subsample the grid to hold cost flat.

**P4 — stop using ISP digital gain as an AE lever.** `maruko_cus3a.c:269,
482-489`. Pin `IspGain=1024` and converge with shutter + analog gain only
(matches vendor priority) — digital gain around the colour stages causes
channel-clip casts and amplifies noise.

**P5 — damp native AWB** (belt-and-suspenders): enable the AWB stabilizer /
lower AWB speed (`AWB_INVESTIGATION.md`; currently disabled) so it can't chase
fast transients. Mostly redundant once P1 lands.

**P6 — raise `AE_TARGET_Y`** from 80 toward the vendor operating point
(native converges much brighter) so throttle images aren't dim vs native.
`maruko_cus3a.c:263`.

**Also:** `documentation/AE_AWB_CPU_TUNING.md:118-124` is **stale** — it claims
Maruko never drives AE in userspace, which the throttle path contradicts. Fix
it when P1/P2 land.

## Recommended path
Do **P1 + P2 + P4 together** (the actual defect fixes; P1=stepping, P2=colour,
and P1 also removes the AWB wobble). Then **P3 (30 Hz)** and **P6 (target)** as
tuning, **P5** only if HW testing still shows wobble. Net CPU delta is
dominated by P3 — a few points of one core at 30 Hz — so the ~10-point throttle
win is preserved while the quality approaches the smooth Star6E feel.

Key files: `src/maruko_cus3a.c` (control law + metadata), `src/venc_config.c:97`
(aeFps default), reference `src/star6e_cus3a.c:288-468` (the supervisory-only
pattern).

---

## Session followup — status & unconcluded work (2026-07-04)

### Shipped this branch (PR #156)
- **P1 (IIR-damped proportional control)** — `maruko_cus3a.c`. Replaced the
  bang-bang step cascade with a log-domain IIR (`e_next = e·ratio^0.20`,
  ratio-clamped 4×, 1.5% deadband, split shutter→analog). Device A/B: the new
  custom AE now looks "very similar to the SDK" (user-confirmed) — the visible
  stepping is gone. Committed `9a48c21`.
- **P4 (pin ISP digital gain)** — `cur_isp_gain = AE_GAIN_MIN`; converge on
  shutter+analog only. Committed `9a48c21`.
- **majestic-parity IQ path** — re-enabled `MI_ISP_IQ_ApiCmdLoadBinFile`
  (user_key 1234) + made the global bypass-OFF loop opt-in
  (`MARUKO_IQ_BYPASS_ALL=1`). Committed `3a4ad62`. Both ~0 CPU delta.

### Grain — root-caused, NOT the IQ path
The grainy-vs-majestic image was **channel-level 3DNR off**:
`isp_para.level3DNR` is driven by `fpv.noiseLevel`, device had it at **0** →
3DNR disabled → grain. Setting `noiseLevel=3` gave a clean image matching
majestic (user-confirmed "noticeably better"). **Cost:** enabling channel-level
3DNR raised waybeam CPU 68→75% at 100fps (its 3DNR runs in the ISP driver's
per-frame path). **Open decision:** do NOT bump the shipped `fpv.noise_level`
default (0) — it trades against the CPU-gap goal. majestic gets clean NR at
*lower* CPU on the **same** ISP driver, so the right fix is matching majestic's
ISP 3DNR configuration, not a pipeline change. Documented, not changed in code.

### OPEN — blocks the remaining AE tuning
- **avgY metering bug (must fix first).** The tick log shows `avgY ≈ 4` on a
  *lit* bench scene — the average is being over-divided. Suspect the block-count
  (`ae_info.AvgBlkX * AvgBlkY`) reads 0 and falls back to `MARUKO_AE_GRID_SZ`,
  or the sum/scale is wrong. **Until avgY is trustworthy, P2/P6 (target-driving
  and BV-for-colour) are meaningless** — you cannot tune a target against a
  broken measurement. This is the next concrete task.
- **"~150 delta ticks" question (unresolved).** The cus3a tick counter jumps
  ~150 between 5 s prints. Never fully explained — likely just the per-frame
  tick rate over the interval, but confirm it's not double-ticking. Cheap to
  close once metering is trusted.
- **P2 (real BV for colour)** — still needs HW calibration (capture native BV at
  3–4 light levels, fit the encoding). Gated on avgY fix.
- **P3 (aeFps 15→30), P5 (AWB damp), P6 (raise AE_TARGET_Y)** — untouched;
  polish after P2. P6 also gated on trustworthy avgY.

### CPU gap — NOT architectural (VPE-port premise falsified)
waybeam 68% vs majestic 43% at 1080p100 is a **config difference within the
identical `MI_ISP`+`MI_SCL` pipeline**, not a VPE-vs-discrete architecture split.
The `feature/maruko-vpe-pipeline` investigation proved I6C has no VPE deployed
(no `libmi_vpe.so`/`mi_vpe.ko`; the SDK's `libmi_vpe` is a 47 KB shim over
`MI_ISP_*`), and majestic runs the **same** mode (plus a 2nd h264 stream + audio)
on the **same** discrete path at 43%. The gap concentrates in `IspMidThreadWq`
(+13pt, majestic ~0), `isp0_P0_MAIN` (+9.6pt), and the main loop (+8.6pt) —
`3A_Proc_0` is *higher* on majestic, so AE-throttle is not this lever. Chase what
re-triggers `IspMidThreadWq` per frame (3DNR reconfig / per-frame
`MI_ISP_SetChnParam` / CUS3A shadow push) vs majestic. Full writeup:
`documentation/MARUKO_VPE_PORT_INVESTIGATION.md` (on the vpe-pipeline branch).
