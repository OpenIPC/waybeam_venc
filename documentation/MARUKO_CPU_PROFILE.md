# Maruko IMX415 — CPU profile at high fps (90 / 100 fps)

Investigation of the sustained ~50–60% CPU observed at the 90 and 100 fps
modes. Device: SSC378QE Maruko, **single-core ARMv7** (so all percentages are
of one core). Method: `/proc/<pid>/task/*/stat` utime+stime jiffie deltas over
a 10 s window + `/proc/stat` for whole-system busy
(`scripts/maruko_cpu_profile.sh`, run on-device: `sh maruko_cpu_profile.sh
[interval_s]`), while streaming H.265 at ~16 Mbps.

This is orthogonal to the mode-lineup change on this branch — it characterises
the pre-existing per-frame cost of the pipeline. No code behaviour is changed
here; the recommendations below are proposals.

## Measured

| Config | System busy | `3A_Proc_0` | main thread | other |
|---|---|---|---|---|
| mode 4, 100 fps, native AE | **62.3%** | 16.7% | 12.3% | 0.7% |
| mode 3, 90 fps, native AE | **60.0%** | 15.3% | 12.5% | 0.6% |
| mode 3, 90 fps, **throttle AE** | **49.9%** | 11.3% | 12.2% | 1.4% |

90→100 fps only moves ~2 points: at these rates the pipeline is near a high
fixed baseline, not scaling linearly in that narrow range.

> **Load-average red herring:** `top` shows load ~14 on this 1-core SoC. That
> is the SigmaStar SDK's kernel pipeline threads (`venc0_P0_MAIN`,
> `IspDriverThread`, `SensorIfThreadW`, `scl0_P0_MAIN`, `vif3_P0_MAIN`, …)
> sitting in uninterruptible **D-state** waiting on hardware. D-state counts
> toward load average but consumes ~no CPU. Ignore load average here; use the
> jiffie-delta busy% above.

## Where the ~60% goes — system-wide per-thread (corrected)

A **system-wide** thread scan (not just waybeam's threads — earlier drafts of
this doc mis-bucketed the non-waybeam kernel time as a vague "irreducible
pipeline"; it is not vague and not all irreducible). At 90 fps native AE,
61.7% busy, `usr=7.6% sys=51.6% softirq=0.5%` — the cost is **kernel/system
time**, and it is the ISP that dominates:

| Thread | CPU% | What it does (per sensor frame, 90 Hz) |
|---|---|---|
| `3A_Proc_0` | **16.0%** | SDK **userspace** 3A — runs the full AE+AWB algorithm every frame |
| `isp0_P0_MAIN` | **15.9%** | ISP pipe-0 kernel thread — captures/reads the 3A HW statistics + runs 3A math |
| `waybeam` (main) | **13.1%** | `MI_VENC_GetStream` → HEVC/RTP → `sendmmsg` + scene-size + IMU drain |
| `IspMidThreadWq` | **11.1%** | ISP "mid" kernel workqueue — **control-plane**: programs 3DNR config + loads CMDQ register banks + manages 3DNR ref-buffers + pushes exposure over I2C + applies per-frame IQ shadow regs |
| `vif0_P0_MAIN` | 2.4% | VIF |
| `venc0_P0_MAIN` | 2.2% | VENC |

So the **ISP + 3A subsystem ≈ 43%** of the core (`3A_Proc_0` + `isp0_P0_MAIN`
+ `IspMidThreadWq`); VIF+VENC are trivial (4.6%) and the encoder/output is
only 13%. **The ISP, not the encoder, is the CPU story.**

**SDK ground truth** (from the I6C SDK at `/home/snokvist/dev/Maruko`,
`libispalgo`/`libmi_isp` + RTOS symbol map):

- **No pixel filtering runs in software.** 3DNR temporal filtering is
  fixed-function **VPE hardware** (`DrvIsp_IsHwPost3DNR`). What `IspMidThreadWq`
  spends 11% on is per-frame *control-plane* work — 3DNR config computation,
  CMDQ bank loading, ref-buffer DMA management, I2C exposure writes, IQ shadow
  register updates. All of it repeats at sensor rate, so **CPU scales linearly
  with fps** (90→100 only moved 2 pts because it is already near a high fixed
  floor).
- **There is no firmware/coprocessor 3A on I6C** — `DoAe`/`DoAwb` are ordinary
  ARM functions in `camdriver.lib`. AE/AWB genuinely run on the single
  Cortex-A7. Maruko is *forced* onto the userspace-CUS3A path
  (`MI_ISP_EnableUserspace3A`, `maruko_pipeline.c:184-231`) — without it the
  ISP FIFO stalls at ≥60 fps — which is why we pay **both** `isp0_P0_MAIN`
  (kernel stats/servicing) **and** `3A_Proc_0` (userspace AE/AWB), plus the
  `IspMidThreadWq` bridge that shuttles stats to userspace each frame.
- The encode/output main thread is **already optimal** (see below) — no
  high-value cut there.

## What is already efficient (verified by code audit)

- **RTP output path** (`maruko_output.c`, `rtp_packetizer.c`, `hevc_rtp.c`):
  zero-copy — the payload stays an iovec pointer into the encoder buffer, only
  a 15-byte RTP+FU header is copied per packet, and **all packets of a frame
  go out in one `sendmmsg`** (~100 send syscalls/s at 100 fps, not per-packet).
  No per-frame heap allocation, no spin, no per-byte CRC. Nothing to gain.
- **Encode fetch loop** (`maruko_pipeline.c:3311`): blocks on `poll()` of the
  VENC fd — wakes at fps, does not spin. The 1000 ms timeout is only a
  shutdown-latency cap.
- **IMU** (`imu_bmi270.c`): default is FIFO mode with **no polling thread** —
  drained once per frame (2 I2C transactions). Avoids the known 1600 Hz
  I2C death-spiral. Only the *fallback* path (FIFO init failed) spins at ODR.
- **Scene detector** (`scene_detector.c`): integer EMA over encoded byte
  counts, no pixel/histogram work. Negligible.
- **Verbose log + CPU sample**: gated to ~1 Hz / 500 ms. Negligible.

## Recommendations (prioritised)

### 1. Enable the AE throttle at high fps — **measured 60.0% → 49.9%**
The throttle (`isp.aeEngine = custom`, default `sdk`) swaps the SDK's
per-frame AE algorithm for a stub and runs the AE control law in a fixed
~15 Hz supervisory thread instead (`src/maruko_cus3a.c`, dispatch
`maruko_pipeline.c:2333-2362`). Measured **~10 points off the whole system**
at 90 fps (bigger than the 4-pt `3A_Proc_0` drop, because it also sheds the
associated kernel ISP-stats/i2c work). AWB stays per-frame native by design
(no colour cast); only AE responsiveness relaxes to 15 Hz, which is fine for
FPV.

- Config key: `isp.aeEngine` = `sdk` | `custom`; companion `isp.aeFps`
  (default 15) sets the supervisory rate.
- **Proposal:** auto-select `custom` when the active sensor mode fps ≥ ~60,
  keep `sdk` for low-fps modes (per-frame AE is cheap there and native
  quality is free). This makes 90/100 fps ship at ~50% instead of ~60%
  without a config change, and leaves 30/50 fps untouched.

### 2. IQ-API NR/WDR bypass — **MEASURED ~0, do NOT pursue**
Device A/B at 100 fps (mode 4), native AE: disabling `nr3d`, `nr_despike`,
`nr3d_ex`, `wdr` via the IQ API (`MI_ISP_IQ_SetNr3d(enabled=false)` etc.,
`src/maruko_iq.c:136-179`) moved system busy **66.2% → 65.3%** (≈1 pt, within
noise) and `IspMidThreadWq` **13.0% → 12.9%** (unchanged). Surprisingly these
modules ship **enabled** even in the linear mode (WDR=128, WDR-LTM/NR on,
3DNR on) — but the IQ enable flags tune the module's *effect strength*, not the
per-frame ISP control-plane work (`IspMidThreadWq` still computes 3DNR config +
loads CMDQ banks + manages ref-buffers regardless). **Conclusion: the easy IQ
bypass is a dead end for CPU.**

Untested variant: the *channel-level* `MI_ISP_SetChnParam e3DNRLevel = OFF`
(`mi_isp.h` — a different API than the IQ `nr3d` flag) plus
`MI_ISP_IQ_SetEnSysMcnrMemory(off)` might disable the 3DNR *engine* (and its
ref-buffer traffic) where the IQ flag does not. Neither is wired in our code;
given the IQ result above, expectations should be low until a prototype
measures it.

### 3. Shrink the 3A statistics grid — untested, uncertain
The AE stats grid is up to 128×90 = 11 520 blocks that the SDK 3A and our
supervisory loop iterate every frame. `MI_ISP_CUS3A_SetAEWindowBlockNumber`,
`SetAEHistogramWindow`, `SetAWBSampling` (`mi_isp_cus3a_api.h`, **not wired** in
our code) reduce it. This is the only remaining "keep native AE quality, cut
per-frame cost" lever, but given that the NR/WDR bypass moved nothing, temper
expectations — much of `isp0_P0_MAIN` may be fixed servicing, not grid-size
dependent. Needs a code prototype + on-device A/B before relying on it.

### Note — no SDK knob to run native 3A every Nth frame
Searched the CUS3A/AE/ISP API: there is **no** parameter to decimate the SDK's
native 3A to every-Nth-frame (the closest are `MI_ISP_AE_SetConverge`, which
tunes convergence *conditions* not *rate*, and `MI_ISP_SkipFrame`, a
pipeline/startup frame-skip, not 3A decimation). Per-frame 3A is baked into how
the ISP driver services frame-end. So "keep the good SDK AE but run it less
often" is not available — the only way to stop per-frame AE math is the
userspace stub (the throttle), which is why fixing the throttle's quality
(`MARUKO_AE_IMPROVEMENT_BRAINSTORM.md`) is the real path to CPU + quality.

### 5. Debug OSD redraw + IMU fallback (secondary)
- Gate the debug-OSD redraw to ~5–10 Hz when enabled — it re-rasterises +
  `MI_RGN_UpdateCanvas` every frame though values change ≤1 Hz
  (`maruko_pipeline.c:3538-3607`). Zero cost when OSD is off (the default).
- Clamp the IMU *polling-fallback* rate (default FIFO path is fine; the
  fallback does one I2C txn/sample at `imu.odr`, up to 1600 Hz).

### What is NOT a lever (I6C silicon limits)
- **Bind mode.** FRAMEBASE on the ISP→SCL edge is rejected by `mi_sys`
  (`-1610014712`, `maruko_pipeline.c:691-694`) — REALTIME is forced.
- **ISP-internal downscale** (process at encode size, not sensor width): a
  non-zero ISP output-port crop *stalls* ISP processing at high res
  (`maruko_pipeline.c:595-598, 689-690`); the ISP must process full width.
- **Offloading 3A/mid-stage to a coprocessor:** there is no firmware 3A on
  I6C; it runs on the ARM. Cannot be delegated, only run less often / smaller.
- **Lower fps / resolution** is the only lever on the fixed floor, and it
  changes the product.

## Why Star6E (SSC338Q) does 8–10% and Maruko can't (honestly)
This gap is **mostly structural/hardware, not a bug we can close to parity**:

1. **Single-core vs (reportedly) dual-core + higher clock.** SSC378QE/I6C is a
   single Cortex-A7; SSC338Q/I6E is a higher-clock, likely dual-core part —
   the same absolute ISP work is a much smaller fraction of "one core," and I6E
   can spread pipeline + 3A across two cores. *(Confirm the I6E core count on
   the actual Star6E device; it materially changes the comparison.)*
2. **fps.** The pipeline is strictly per-frame, so Maruko at 90–100 fps already
   costs ~1.5–1.7× an I6E setup tested at 60 fps.
3. **Forced userspace 3A.** Maruko must keep userspace CUS3A on (FIFO stalls
   otherwise), paying the `3A_Proc_0` + `IspMidThreadWq` bridge that an in-kernel
   3A path avoids.
4. **Config** (3DNR, 3A grid, IQ modules) — the only genuinely recoverable part
   (recs 2–4), at an image-quality cost.

Note: the earlier "Star6E hands AE to ISP firmware (near-zero ARM)" hypothesis
is **not supported** by the I6C SDK (no firmware 3A exists there); treat any
I6E firmware-3A claim as unverified until checked on the I6E SDK/device.

## Bottom line
The ~60–66% is ISP-dominated: ISP+3A ≈ 45% of the core, encoder/output ~13%,
VIF+VENC trivial. The **only measured CPU lever is the AE throttle**
(60%→50% at 90 fps). The IQ-API NR/WDR bypass was **measured at ~0** (66.2→65.3,
noise) — a dead end. There is no SDK knob to run native 3A every Nth frame. The
3A-grid-shrink is unwired and, after the NR/WDR null result, uncertain. So the
practical conclusion is narrow: the per-frame ISP/3A cost on this single-core
I6C is largely fixed, and the one real win — the throttle — comes with an AE
quality cost. **Making the throttle's AE not-ugly
(`MARUKO_AE_IMPROVEMENT_BRAINSTORM.md`) is therefore the highest-value work:
it's the only path that buys both the ~10 CPU points and acceptable image.**
Reaching I6E's 8–10% is not achievable on this silicon.
