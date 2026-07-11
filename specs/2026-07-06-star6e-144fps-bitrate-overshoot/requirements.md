# Star6E IMX335 144 fps mode — CBR bitrate overshoots ~4.7×

- **Repo:** `waybeam_venc`
- **Branch introducing the mode:** `feature/star6e-imx335-imx415-sensor-modes` (mode not yet on `master`)
- **Sensor mode:** IMX335 Star6E `LINEAR_RES_6` = `1600x900@144fps`, idx 5
  (`drivers/sensor_imx335_star6e.c:156`)
- **Status:** ✅ **RESOLVED & device-verified** — see "Resolution" below.
  Real root cause is a **hard 120 fps VENC encoder-input ceiling**, not the
  SetFps-fallback theory first recorded here (kept below for the record).
- **Addendum (2026-07-09, v0.38.0, bench `.201`):** the residual "~143.x,
  wanders 142–143.5" delivery was a second, independent defect — the stock
  driver **extends VMAX** when AE requests exposure ≥ the frame budget
  (1880→1887 with AE pinned at the 1/144s ceiling), plus a ~0.2–0.5% real
  pixel-clock deficit vs the K constant. Fixed in the driver by clamping
  exposure to `vts-9` (fixed-framerate policy) and trimming the mode-5 seed
  1880→1875. Verified: VMAX register pinned, VIF ~144.0–144.3, encoder
  `Fps_1s` 144.00 sustained, wire ≈ 1.19× set bitrate as designed.
  The **"Follow-up (optional)" exact-CBR compensation is now IMPLEMENTED**
  in the same release: budget ×rc_fps/delivered at create
  (`star6e_pipeline.c` venc_max_rate), live `apply_bitrate`, and
  factor-change re-apply in `apply_fps`. Device-verified: 19.6 Mbps wire
  vs 20 Mbps config at 143.99fps (previously 23.8 Mbps). The old QP
  saturation concern only applied when RC wrongly budgeted 30fps.
- **Device under test:** SSC338Q / Infinity6E ground-truth unit, `10.6.0.1`
  (`waybeam` daemon on `:80`, `flashd` on `:8070`), 2026-07-06

## Symptom

On the 144 fps IMX335 mode only, the encoder emits **~4.7× the configured
`video0.bitrate`**. Every other mode (30/60/90/100/120 fps) holds CBR correctly.

| `video0.fps` | encoder `SrcFrmRate` | RC `GOP` | measured wire kbps | verdict |
|---|---|---|---|---|
| 60 | 60/1 | 120 | 3149 | ✓ (target 3000) |
| 100 | 100/1 | 200 | 3189 | ✓ |
| **144** | **30/1** | **60** | **14116** | ✗ ≈ 4.7× |

(`video0.bitrate=3000`, `rcMode=cbr`, `gopSize=2 s`, `qpDelta=-12` throughout.)

`qpDelta` is a *minor* secondary contributor only: at fps=144, `qpDelta=-12`
→ 15454 kbps vs `qpDelta=0` → 12716 kbps. The dominant term is the fps
mismatch below.

## Resolution (device-verified 2026-07-06, `10.6.0.1`)

### Real root cause — VENC 120 fps hard ceiling

The Infinity6E VENC block **rejects any encoder input frame rate > 120 fps and
silently resets it to 30/1**. Kernel log at boot into the 144 mode:

```
[MI ERR]: _MI_VENC_VerifyFps[3251]: Input invalid FPS:144/1 is over 120, set 30/1 by default
```

So the encoder's rate-control fps was reset to **30** while the VPE→VENC bind
delivered ~143 frames/s → CBR budgeted `3000/30` per frame and emitted 143 of
them → `143/30 ≈ 4.7×` overshoot. Boundary probe confirms the exact ceiling:

| set fps | VENC accepts | SrcFrmRate | wire kbps |
|---|---|---|---|
| 120 | yes | 120/1 | 3059 ✓ |
| 121 | no → reset 30 | 30/1 | 11971 ✗ |
| 144 | no → reset 30 | 30/1 | ~14100 ✗ |

The `state->sensor.fps == 30` value (SetFps-fallback, described in the original
analysis below) is a *red herring*: it happens to equal the VENC default, but
even forcing the encoder-create fps to 144 (`venc_fps` fix) does **not** help —
144 is simply rejected by `_MI_VENC_VerifyFps` and reset to 30 anyway. The
binding constraint is the 120 fps hardware ceiling.

### Key correction — the VENC DOES encode 143 fps

The 120 limit is on the **rate-control fps _parameter_ only**, NOT on encode
throughput.  `/proc` shows `VENC Fps_1s = 143` — the block genuinely encodes
143 fps.  `_MI_VENC_VerifyFps` merely refuses to let you *tell the rate
controller* a number > 120, and resets that parameter to 30.  So a "cap the
output to 120 fps" fix (an earlier attempt) needlessly throttles a stream the
hardware can actually produce.  Two dead ends ruled out on device:

- **Cap delivery to 120 fps** — works (clean CBR) but gives a 120 fps stream,
  not the 144 the mode promises.  Rejected: throughput isn't the constraint.
- **Linear bitrate compensation** (`bitrate × rc_fps/actual`) — NOT robust: at
  a tiny per-frame budget the encoder hits its QP ceiling and can't compress
  that small, so the `output = bitrate × actual/rc_fps` model breaks.  Measured:
  `fps=144, bitrate=629 (=3000×30/143)` produced **3738 kbps**, not ~3000.

### Fix (implemented & device-verified) — decouple delivery from RC fps

Deliver the **true** sensor rate to VENC (the encoder outputs 143 fps) but cap
only the **rate-control `fpsNum`** to `STAR6E_VENC_INPUT_FPS_MAX = 120`
(`include/star6e_pipeline.h`) so `_MI_VENC_VerifyFps` never resets it to 30.
`rc_fps = 120` vs a 143 delivered rate leaves only ~19 % CBR overshoot (`143/120
= 1.19×`) with QP in its normal regime — vs the ~4.7× before.  Sites:

- `src/star6e_pipeline.c` — create-path `venc_fps` (RC) capped to 120; the two
  `bind_dst_fps` computations deliver the TRUE rate (no 120 cap); GOP derived
  from the capped RC fps (`2 s × 120 = 240` frames ≈ 1.68 s at 143 fps).
- `src/star6e_controls.c` `apply_fps()` — bind DST = true fps (delivers 143),
  `apply_encoder_fps(rc_fps)` with `rc_fps = min(fps, 120)`.

### Verified after fix (config `fps=144`, `bitrate=3000`, same scene)

```
RC SrcFrmRate 120/1   actual Fps_1s 143.0   RC_GOP 240   no fps reset
wire ≈ 4080–4188 kbps   (fps=60 baseline on same scene ≈ 3300)
```

True 144 fps output, ~1.2–1.25× over target (RC budgets for 120, emits 143).

### Follow-up (optional, for exact CBR)

To remove the residual ~20 % overshoot, compensate the encoder bitrate by
`rc_fps / delivered_fps` (= 120/143 ≈ 0.84) wherever the CBR target is set
(create `venc_max_rate`, live `apply_bitrate`, and re-applied on `apply_fps`
since the factor tracks fps).  This is safe here — unlike the failed low-budget
test, `rc_fps=120` keeps the per-frame budget out of the QP-saturation regime.
Deferred: 3 call-sites + a stored factor in the control ctx; the core "true
144 fps" behaviour already works without it.

### Note on mode labelling

The mode now genuinely delivers ~143 fps, so the `1600x900@144fps` label is
honest.  No relabel needed (supersedes the earlier "must relabel to 120"
note).

---

## Original analysis (superseded — kept for the record)

## Root cause (confirmed)

The encoder's **rate-control frame rate is 30** while frames actually flow at
**~143 fps**, so CBR budgets each frame for a 1/30 s slice and then emits 143
of them per second:

```
overshoot ≈ actual_fps / rc_fps = 143 / 30 = 4.77×   →   3000 × 4.77 ≈ 14300 kbps
```

Device evidence (`/proc/mi_modules/mi_venc/mi_venc0`, fps=144 state):

```
InputPort of dev 0:  SrcFrmRate 30/1         (rate controller believes 30 fps)
chn0 state:          Fps_1s 143.06  kbps 13828
RateCtl:             CBR  GOP 60  MaxBitrate 3072000  IPQPDelta -12
```

`/proc/mi_modules/mi_vif/mi_vif0`: VIF delivering **141.15 fps, DropCnt 0** —
the **sensor is NOT the bottleneck**. The earlier "sensor can't keep 144 /
not enough headroom" hypothesis is disproved: VIF holds 143 fps cleanly. The
overshoot is purely the encoder rate-control fps being stuck at 30.

`GOP 60` (= `gopSize 2 s × 30`) confirms the encoder was **created** with
fps=30 — the 30 is baked in at pipeline build, not just a live-apply artifact.

## Why the encoder fps is 30 — two divergent fps sources

There are two independent "sensor fps" values in the Star6E path, and they
**disagree** for the 144 tier:

1. **Live-apply clamp source** — `g_star6e_control_ctx.sensor_fps` is set from
   `pipeline->sensor.mode.maxFps` (`src/star6e_controls.c:1197`). For the 144
   mode `maxFps = 144` (driver table `{1600,900,3,144}` → `max_fps=144`).
   This is why live `apply_fps(60)` / `apply_fps(100)` succeed: `fps < 144`,
   no clamp, `apply_encoder_fps()` writes `fpsNum = 60/100`, and `SrcFrmRate`
   updates accordingly.

2. **Encoder-create source** — the encoder is built from
   `venc_fps = min(video0.fps, pconf.sensor_framerate)`
   (`src/star6e_pipeline.c:2420-2422`), and
   `pconf.sensor_framerate = state->sensor.fps` (`src/star6e_pipeline.c:1715`).
   For the 144 mode `state->sensor.fps` resolves to **30** at build time, so
   `venc_fps = min(144, 30) = 30` → `SrcFrmRate=30`, `GOP=60`.

So `sensor.mode.maxFps = 144` but `state->sensor.fps = 30` for the same mode.
The encoder is born at 30; the live path *thinks* the ceiling is 144.

## Why live `apply_fps(144)` fails to repair it

`apply_fps()` (`src/star6e_controls.c:340`) clamps the request to
`sensor_fps = 144`, then rebinds `VPE→VENC` FRAMEBASE at `src=144, dst=144`
and only then calls `apply_encoder_fps(fps)`. For the 144 request the
rebind (or the encoder-fps write) fails, so the **restore path runs and the
stale fpsNum=30 is retained** (return −1, no `apply_encoder_fps`):

```c
bind_ret = MI_SYS_BindChnPort2(..., sensor_fps, fps, FRAMEBASE, 0);
if (bind_ret != 0) { /* restore sensor_fps:sensor_fps, return -1 */ }
if (apply_encoder_fps(fps) != 0 || apply_scene_fps(fps) != 0) { /* restore, return -1 */ }
```

`apply_fps(60/100)` rebind to a *lower* dst succeeds → fpsNum rewritten →
correct CBR. `apply_fps(144)` (dst == sensor max) does not → 30 persists.
(Exact failing call — the `src=144:dst=144` rebind vs the encoder-fps write —
is the one open item; capture the `> Rebind ... failed` / `> Encoder fps
apply failed` stderr line on-device to disambiguate.)

## Relationship to the earlier GOP work

Prior work made **GOP** scale with the *live* fps
(`pipeline_common_gop_frames(gop_seconds, fps)` +
`venc_api.c:1521-1538`, which prefers `query_live_fps()` so a 144-request
clamped to the running fps gets the right I-frame interval). That fix is
correct and unrelated — it operates on whatever fps the encoder actually
reports. The defect here is upstream of it: the encoder's fps itself is 30,
so both GOP (60) and the CBR bit budget inherit the wrong base.

## Fix direction (not yet implemented)

Primary: make the encoder-create fps for the 144 tier equal the mode's real
`maxFps`, not the cold-boot-locked `state->sensor.fps`. Candidates:

- At build, derive `venc_fps` from `sensor.mode.maxFps` (the same source the
  live-apply clamp trusts) rather than `state->sensor.fps`, OR ensure
  `state->sensor.fps` is re-synced to the post-cold-boot-re-kick rate before
  `star6e_pipeline_start_venc()` reads it. See the cold-boot fps re-kick at
  `src/star6e_pipeline.c:364-382` — the re-kick bumps VIF to 143 but the
  encoder params captured 30 earlier.
- Secondary: make `apply_fps(fps == sensor_fps)` a no-fail path (or
  re-issue `apply_encoder_fps` even when the FRAMEBASE rebind to `dst==src`
  reports the "no change" condition) so a live re-apply can self-heal a
  mis-created encoder without a respawn.

Do **not** "fix" by lowering the mode to a rate the encoder tolerates — the
encoder demonstrably sustains 143 fps at correct CBR once fpsNum matches.

## Validation plan

1. Reproduce: fresh boot into the 144 mode, `video0.bitrate=3000`,
   read `/proc/mi_modules/mi_venc/mi_venc0` → expect broken `SrcFrmRate 30/1`,
   `GOP 60`, wire ≈ 14 Mbps.
2. Apply fix, rebuild (`make verify` — build both platforms per repo policy).
3. Post-fix on `10.6.0.1`: `SrcFrmRate 144/1`, `GOP 288` (2 s × 144),
   wire ≈ 3000 kbps at fps=144. Sweep 60/100/144 — all within ±10 % of target.
4. Regression: 30/60/90/100/120 modes still hold CBR (they already do).

## Reproduction commands (device `10.6.0.1`)

```sh
# rate-control fps the encoder is using
awk '/InputPort of dev/{f=1} f&&/^    0 /{print "SrcFrmRate="$4; exit}' \
    /proc/mi_modules/mi_venc/mi_venc0
# actual encoded fps + kbps
awk '/ChnId  State  EnPred/{f=1;next} f&&/^    0 /{print "Fps1s="$9" kbps="$10; exit}' \
    /proc/mi_modules/mi_venc/mi_venc0
# true wire rate (stream egress is wlan0 → 192.168.2.242:5600)
A=$(awk '/wlan0/{gsub(/:/,"",$1);print $10}' /proc/net/dev); sleep 4; \
B=$(awk '/wlan0/{gsub(/:/,"",$1);print $10}' /proc/net/dev); \
echo "$(( (B-A)*8/1000/4 )) kbps"
```
