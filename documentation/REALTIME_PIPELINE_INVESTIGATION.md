# Pure REALTIME Pipeline — Investigation & Star6E/IMX335 MVP Plan

Date: 2026-07-17 | Status: **Investigation complete — implementation plan
ready, pending approval** | Supersedes the plan section of
`LOW_DELAY_PIPELINE.md`.

## Goal

Switch the encode path to a *pure* realtime chain — every inter-module link
streaming (REALTIME/RING) instead of frame-buffered (FRAMEBASE) — so the
encoder starts consuming rows before the full frame lands in DRAM.
Estimated gain (from `LOW_DELAY_PIPELINE.md` measurements): capture-to-wire
latency from ~5.9 ms down to ~3 ms at 90 fps; proportionally similar at 120.

MVP target: **Star6E (SSC338Q / Infinity6E) + IMX335**, one verified
realtime-compatible sensor mode.

## 1. What "pure REALTIME" means per link

SigmaStar `MI_SYS_BindChnPort2` link types (device-confirmed values in
`MAJESTIC_MI_IOCTL_MAP.md`): `FRAMEBASE=1` (full frame staged in DRAM, m2m
consumption, ~+1 frame latency), `REALTIME=4` (on-chip line-buffered
streaming), `RING=0x10` (line-ring in DRAM, consumer chases producer rows —
the streaming mode the vendor uses into VENC).

## 2. Current state — the two backends

### Maruko (I6C) — already pure realtime

`src/maruko_pipeline.c`:

| Link | Mode | Where |
|---|---|---|
| VIF→ISP | **REALTIME** (FRAMEBASE only for `_1485` modes) | `:2443` |
| ISP→SCL | **REALTIME** | `:2458` |
| SCL→VENC | **LINK_RING** | `:2468` |

Supporting pieces that make the RING leg work on I6C:
- VENC input source `I6C_VENC_SRC_CONF_RING_DMA` set between `CreateChn`
  and `StartRecvPic` (`:2089`), matching the OpenIPC i6c HAL reference
  (`sdk/ssc338q/hal/star/i6c_hal.c:688`).
- Device ring pools via `MI_SYS_ConfigPrivateMMAPool` for SCL (`:2355`,
  ring = capt_h/4) and VENC (`:2044`, ring = full height).
- SCL output port `compress = 6` (IFC) — required for the RING bind
  (`:875`; the stab-fill NORMAL path sets 0).
- RING is **1:1**: a second bind on the same SCL port returns busy
  `0xA0092012`, which is why the JPEG snapshot taps a *second* SCL port in
  FRAMEBASE @5 fps (`src/maruko_jpeg.c` header comment).
- Live fps divider works by re-binding SCL→VENC with `sensor_fps:fps`
  still in LINK_RING (`src/maruko_controls.c:314-327`).
- Teardown hardening for in-flight RING/REALTIME buffers (drain before
  unbind + watchdog) — HISTORY 0.40.x, `maruko_pipeline.c:4423-4470`.

### Star6E (I6E) — realtime except the last leg

`src/star6e_pipeline.c`:

| Stage | Mode | Where |
|---|---|---|
| VIF work mode | `I6_VIF_WORK_RGB_REALTIME` | `:439` |
| VIF port `frameLineCnt` | 0 (full-frame notify) | `:478` |
| VPE channel mode | `I6_VPE_MODE_REALTIME` | `:518` |
| VIF→VPE | **LINK_REALTIME** | `:1934` |
| VPE→VENC ch0 | **LINK_FRAMEBASE** ← the gap | `:2005` |

So Star6E is one link away from pure realtime. But that link has four
additional FRAMEBASE call sites that must stay consistent with it:

1. Live-fps rebind (3 bind calls): `src/star6e_controls.c:388-415`.
2. Stab framing port0→VENC bind: `src/star6e_framing_stab.c:1551`.
3. Dual VENC ch1 (recording): `star6e_pipeline.c:2582` — same
   `vpe_port` (port 0) fan-out, deep (8,56) buffer for SD stalls.
4. JPEG snapshot: `src/star6e_jpeg.c:110` — same `vpe_port` fan-out,
   FRAMEBASE @5 fps.

## 3. Why previous attempts failed — corrected record

Three separate failures created the impression "realtime doesn't work for
our modes". None of them is a blocker today:

1. **RING input + FRAMEBASE bind combo (Star6E).** The pipeline stall in
   the original low-delay test came from setting the VENC ring input mode
   while the bind stayed FRAMEBASE — an invalid combination
   (`LOW_DELAY_PIPELINE.md` §2). `MI_VENC_SetInputSourceConfig` itself
   returns 0 on I6E firmware.
2. **VIF→VPE "bindmode 4 not sync err" flood (Star6E).** Root cause was
   the `i6_vpe_chn`/`i6e_vpe_chn` struct-size mismatch (UB), not the
   REALTIME bind. Fixed and cold-boot verified
   (`VIF_VPE_SYNC_ERROR_POSTMORTEM.md`).
3. **REALTIME half-rate collapse at high pixel rates (Maruko).** With
   stock/foreign sensor drivers, modes whose line bursts exceed the ISP
   drain collapse to half fps or storm the ISP P0 FIFO
   (`MARUKO_IMX335_FRAMEBASE_49FPS.md`, `MAJESTIC_MI_IOCTL_MAP.md`). This
   is the "our modes weren't compatible" memory: **realtime compatibility
   is a property of the sensor mode's line pacing (HMAX/blanking/link
   rate), which we couldn't tune without owning the driver.**

What changed: both platforms now have in-tree, buildable drivers
(`drivers/sensor_imx335_star6e.c`, `sensor_imx335_maruko.c`,
`sensor_imx415_*.c`, built via `make drivers-star6e` / `make sensor`) with
Maruko-style explicit-geometry window-crop modes where we control HMAX and
therefore the line-burst rate. The Maruko IMX335 lineup runs **all six
modes REALTIME** on I6C (`MARUKO_IMX335_MODES.md`), and the Star6E IMX335
lineup is device-verified at full rate on the (already-REALTIME) VIF→VPE
leg across all six modes (`STAR6E_IMX335_MODES.md`).

## 4. Implementation plan — Star6E VPE→VENC realtime leg

Smallest-change order, mirroring the proven Maruko/i6c pattern:

### 4.1 Load the missing symbol

`src/star6e_mi.c` (`load_venc_symbols`, `:251-285`) — add
`fnSetInputSourceConfig` = `MI_VENC_SetInputSourceConfig` and a wrapper
macro in the star6e bindings header. The enum already exists:
`i6_venc_src_conf` = NORMAL / RING_ONE / RING_HALF
(`include/sigmastar_types.h:507-510`). Note I6E has **no RING_DMA** (that
is i6c-only); `RING_ONE` is the "one frame ring" mode the low-delay doc
targeted.

### 4.2 Configure VENC ring input

In `star6e_pipeline_start_venc()` (`star6e_pipeline.c:1207`), after
`MI_VENC_CreateChn`, before `StartRecvPic`: set input source config to
`I6_VENC_SRC_CONF_RING_ONE` — **ch0 only**; ch1 (dual) and the JPEG
channel stay NORMAL. Gate this on a config flag (see 4.6).

### 4.3 Switch the ch0 bind

`bind_and_finalize_pipeline()` (`:2005`) and the stab framing bind
(`star6e_framing_stab.c:1551`): `I6_SYS_LINK_FRAMEBASE` →
`I6_SYS_LINK_RING` (`0x10`, defined for i6 in `sigmastar_types.h:149`).
Keep the `bind_src_fps:bind_dst_fps` divider — Maruko proves RING honours
it (`maruko_controls.c`). If RING is rejected by the i6e mi_sys, fall back
to `I6_SYS_LINK_REALTIME` (the low-delay doc's original proposal); decide
by device probe, log the accepted mode.

Update the live-fps rebind path (`star6e_controls.c:388-415`) to re-bind
with the same link type as the boot-time bind (share one constant/helper —
three call sites).

### 4.4 VIF frameLineCnt

`star6e_pipeline.c:478`: `port.frameLineCnt = precrop->h / 4` — only
meaningful once the full chain streams; keep behind the same flag.

### 4.5 Ring pool (open question)

Maruko configures `MI_SYS_ConfigPrivateMMAPool` DEVICE_RING pools for the
RING legs. That symbol/API shape is i6c-era; the i6e (kernel 4.9) mi_sys
may not export it. Resolve it via optional `dlsym` (missing symbol → skip
with a log line, not a fatal error) and observe whether the RING bind
works without it — the low-delay doc's I6E test accepted the input-source
call with no pool.

Similarly, Maruko's RING leg needs SCL output `compress=6` (IFC); Star6E's
VPE port0 currently sets `I6_COMPR_NONE` (`star6e_pipeline.c:571`). Try
NONE first; if the RING bind or first frame fails, probe IFC.

### 4.6 Config gating + port fan-out policy

RING is 1:1 on the producer port (Maruko: `0xA0092012`). Star6E currently
fans out **three** consumers from VPE port 0: VENC ch0, dual VENC ch1, and
the JPEG snapshot channel. Therefore:

- Add a config switch (suggest `video0.lowDelay` bool, default off) so the
  FRAMEBASE path remains the fallback.
- MVP policy under lowDelay=on: **refuse `record.mode=dual` and disable
  the snapshot bind** (init returns -ENODEV → endpoint serves 503),
  mirroring how Maruko's stab-fill refuses dual. Log the refusal.
- Follow-up (post-MVP): move snapshot (and optionally ch1) to a second VPE
  output port, the Maruko pattern. Note VPE port1 is already used by the
  stab detector tap when `framing=stab`, so the snapshot tap would take
  port2 — needs a device probe that port2 exists/scales on I6E.

### 4.7 Teardown & reinit

RING/REALTIME unbind flushes in-flight buffers with an unbounded wait
(Maruko learned this the hard way — HISTORY 0.40.x watchdog). Star6E
already orders `StopRecvPic` → unbind (`star6e_pipeline_stop_dual`,
main stop path); verify the same ordering holds for ch0 under RING and
exercise SIGHUP reinit repeatedly (`SIGHUP_REINIT.md` flow). If a wedge
appears, port Maruko's drain-before-unbind + teardown watchdog.

## 5. MVP sensor mode choice (Star6E / IMX335)

All six in-tree modes already sustain the REALTIME VIF→VPE leg at full
rate (`STAR6E_IMX335_MODES.md` device table), so mode choice is about VENC
row-pacing headroom and usefulness:

| Idx | Mode | MPix/s | Notes |
|---|---|---|---|
| **4** | **1920×1080@120** | 249 | **Primary MVP** — stock table, standard 1080p, VIF-verified 119.4–120.0, RC cap `STAR6E_VENC_INPUT_FPS_MAX`=120 fits exactly, mid-pack pixel rate (I6E handles up to 332) |
| 5 | 1600×900@144 | 207 | Fallback if ring overflows — lowest pixel rate, Maruko-style crop, VENC already proven encoding 144 fps delivered |
| 3 | 2176×1224@100 | 266 | Second data point once MVP passes (Maruko-style crop tier) |

Nothing needs to change in the sensor driver for the MVP. Driver control
matters as the *recovery lever*: if a mode shows line-pacing overflow
under the full realtime chain (FIFO-FULL / half-rate), we can lower HMAX
or retune blanking in `drivers/sensor_imx335_star6e.c` — the exact trick
already used for the IMX415 100 fps timing wall (HISTORY 0.26.0/0.27.0).

## 6. Verification plan (bench .13, `root@192.168.1.13`)

1. `make verify` (both backends), then deploy via
   `scripts/star6e_direct_deploy.sh cycle` with `sensor.mode=4`,
   `video0.fps=120`, lowDelay=on.
2. **Link accept probe:** bind logs show RING (or REALTIME fallback);
   dmesg clean of `not sync err` / FIFO-FULL / FrmLost
   (`--json-summary` dmesg_hits=0).
3. **Rate:** VIF FPS column in `/proc/mi_modules/mi_vif/mi_vif0` ≈120 and
   encoder `Fps_1s` ≈120 — the halving signature is the known
   over-budget symptom and logs nothing.
4. **Latency:** existing measurement (`monotonic_us() -
   stream->packet[0].timestamp` at RTP send, 1 s averages) A/B against
   lowDelay=off. Success ≈ 2–3 ms improvement; regression bar: no fps
   loss over a 60 s soak.
5. **Controls:** live fps change (120→60→120) exercises the RING rebind;
   SIGHUP reinit ×5; mode warm-switch 4→2→4.
6. **Degradations documented:** dual-record refusal and snapshot 503
   under lowDelay verified; both restored with lowDelay=off.
7. Then repeat step 2–4 on mode 5 (144) and mode 3 (100) to map the I6E
   realtime ceiling, and update `STAR6E_IMX335_MODES.md`.

## 7. Risks

- **RING vs REALTIME on i6e VPE→VENC is unproven** — no in-repo vendor
  reference (OpenIPC i6 HAL binds VENC FRAMEBASE; only the i6c HAL uses
  RING+RING_DMA). Mitigation: probe both, flag-gated fallback to
  FRAMEBASE.
- **GetStream timing changes** under streaming input; the main loop's
  poll cadence may need adjustment.
- **Snapshot/dual loss under lowDelay** is a real feature trade until the
  second-VPE-port tap lands.
- **Teardown wedge** (unbounded RING flush) — mitigated by ordering +
  reinit soak; port the Maruko watchdog if needed.
