# Pure REALTIME Pipeline — Investigation & Star6E/IMX335 MVP Plan

Date: 2026-07-17 | Status: **CLOSED — NOT POSSIBLE ON I6E.  Implemented and
bench-verified on branch `feature/star6e-lowdelay-realtime` (PR #187, closed
unmerged), then root-caused against the vendor SDK (§6b): the i6e MHE H26x
core does not advertise ring/realtime input (`SupportRing=0`; only the JPEG
engine and i6c's H26x core do), so a streaming VPE→VENC bind can never
succeed on this SoC.  The implementation was deliberately not merged — a
default-off flag that provably cannot engage is dead weight.  Resurrect the
branch only if SigmaStar ships an mhal/chagall firmware with
`bExternalRingSupport=1` for MHE.** | Supersedes the plan section of
`LOW_DELAY_PIPELINE.md`.

Implementation deltas from the plan below (all on the closed branch, not in
master): the RING→REALTIME probe and the FRAMEBASE fallback (with frame-base
input restore) live in `star6e_pipeline_bind_venc0()`; the negotiated link is
exposed through `star6e_pipeline_venc_link_type()` for the live-fps rebinds;
dual-record under lowDelay *downgrades to single-channel* (same pattern as
the stab downgrade) rather than hard-refusing; and `framing=stab-fill`
exempts itself from ring input because its compose loop manually feeds the
VENC input port (frame-base required, as on Maruko).  No i6e private ring
pool is configured (correct — §6b confirmed it driver-managed).

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
**Vendor convention (SDK stitch/amigos samples, learned in the §6b review):
an HW_RING bind requires `u32BindParam = ALIGN_UP(height, 32)` — the ring
line count; only REALTIME/FRAMEBASE pass 0.**  (Maruko/i6c passes 0 and
works because i6c takes the ring geometry from its `DEVICE_RING` pool
config instead — do not "fix" the working i6c path.)
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

## 6a. Bench result (2026-07-18, `.201` Star6E / IMX335, mode 4 = 1080p120)

Verified on `192.168.2.201` (`.13` was unreachable). **Verdict: this i6e
firmware rejects a streaming VPE→VENC bind — the feature is a safe, correct
no-op here and always falls back to FRAMEBASE.**

1. **Link accept probe — NEGATIVE, as feared in §7.** Both `LINK_RING` and
   `LINK_REALTIME` binds on VPE→VENC are rejected by mi_sys with
   `0xA0092008` (`-1610014712`). `star6e_pipeline_bind_venc0()` then restores
   the VENC input to `NORMAL` (Stop → `SetInputSourceConfig(NORMAL)` → Start)
   and binds FRAMEBASE — confirmed live: `bind_Type=1` (FRAMEBASE) in
   `/proc/mi_modules/mi_venc/mi_venc0`. So pure-realtime is **not available
   on this i6e leg/firmware**; there is no in-repo vendor reference that it
   ever was (the OpenIPC i6 HAL binds VENC FRAMEBASE — only the *i6c* HAL
   uses RING). REALTIME is accepted for VIF→VPE but not VPE→VENC.
2. **Rate / stability:** sustained **120.0 fps** at mode 4; VENC ISR
   counters `IsrBufFullCnt / IsrRingFullCnt / IsrTimeoutCnt` all **0**; no
   `sync err` / `FIFO-FULL` / `FrmLost` in dmesg. `RewindCnt` 0.
3. **Latency A/B — N/A:** both `lowDelay=off` and `lowDelay=on` resolve to a
   FRAMEBASE bind on this firmware, so there is no latency delta to measure
   and **no regression** (input-task pipeline-delay averages unchanged).
4. **Controls:** live fps `120→60→120` rebind clean (shared link-type
   helper, FRAMEBASE); **SIGHUP reinit ×3** all return healthy (0.48.0,
   FRAMEBASE, ~120 fps, zero ISR errors).
5. **Degradations:** the dual-record / snapshot refusal is gated on the
   *actual* negotiated `g_venc_link_type` (not the config intent), so under
   the FRAMEBASE fallback `snapshot.jpg` returns **200** and dual stays
   available — correct. The streaming-only degradation path therefore cannot
   be exercised on this firmware.
6. **Bug found + fixed on-device (branch-only, but a general lesson):**
   `video0.lowDelay` was wired into the file pretty-printer and the
   FIELD/alias table but **missing from `venc_config_to_json_string`**, so
   `/api/v1/config` (and the WebUI) never showed it.  The field died with
   the branch, but the lesson stands: **the cJSON export in
   `venc_config_to_json_string` is a fourth, independent serializer** that
   the new-config-field checklist must include alongside parse,
   pretty-print, and the FIELD/alias table.
7. **Cosmetic:** stdout is block-buffered to the log file while stderr is
   unbuffered, so the fallback trace prints out of causal order (the
   `ring input (RING_ONE)` stdout line lands after the stderr fallback
   warnings). Harmless; an `fflush(stdout)` before the probe would tidy it.

**Bottom line:** the change is safe, flag-gated, default-off, and correct,
but delivers **no latency benefit on the `.201` i6e firmware** — it is
infrastructure + a documented negative result. Mode-5/mode-3 sweeps
(step 7) were skipped: pointless while every mode falls back to FRAMEBASE.

## 6b. Adversarial root-cause (2026-07-18, vendor SDK cross-validated)

Why does the bind *really* fail?  Full adversarial pass against the vendor
Pudding SDK (`~/dev/star6e_SDK`) + live kernel traces.  Verdict: **the i6e
H26x encoder core does not advertise ring/realtime input capability — a
per-SoC hardware/driver-table fact, not a fixable userland precondition.**

The kernel says exactly why (dmesg on `.201` at bind time):

```
_MI_SYS_IMPL_BindChannelPort: Output port MOD7(VPE) DEV0 CHN0 PORT0
  supported bindmask0 [org output 0000001d, org input 00000001],
  Proposed BindType 00000010(HW_RING) → Filtered 00000010
```

- VPE **output** mask `0x1d` = FRAME_BASE|REALTIME|HW_AUTOSYNC|HW_RING — the
  VPE side supports every streaming type.
- VENC **input** mask `0x01` = FRAME_BASE only — the VENC input port is the
  veto.  Intersection = 0 → `MI_ERR_SYS_NOT_SUPPORT` (0xA0092008).

Hypotheses eliminated, in order:

1. **Enum/ABI mismatch — NO.**  This SDK's `MI_SYS_BindType_e` matches our
   `sigmastar_types.h` exactly (FRAME_BASE 0x1, REALTIME 0x4, HW_RING 0x10);
   `MI_VENC_InputSrcBufferMode_e` NORMAL=0/RING_ONE=1/RING_HALF=2 and the
   one-enum `MI_VENC_InputSourceConfig_t` match our call.
2. **Call ordering — NO.**  Vendor recipe (stitch `st_main_stitch.cpp:1844+`,
   amigos `venc.cpp:106-133`): CreateChn → SetInputSourceConfig(RING_ONE) →
   StartRecvPic → BindChnPort2(HW_RING).  Ours is identical.
3. **`u32BindParam` — REAL BUG, but not the cause here.**  Vendor convention:
   HW_RING requires `u32BindParam = ALIGN_UP(height,32)` ring lines
   (REALTIME/FRAME_BASE pass 0).  Our probe passed 0.  Fixed (the probe now
   passes `ALIGN_UP(h,32)`, stored in `g_venc_ring_lines` and reused by every
   rebind) and re-tested on-device: **still rejected, mask unchanged** — the
   capability veto sits above param validation.  The fix stays because the
   probe was vendor-incorrect and would have false-negatived on capable
   silicon.
4. **Missing VPE→VENC private ring pool — RED HERRING.**  The header defines
   `E_MI_SYS_VPE_TO_VENC_PRIVATE_RING_POOL`/`MI_SYS_ConfigPrivateMMAPool`,
   but no vendor sample in the entire SDK ever configures it; the pool is
   driver-managed (`_MI_VENC_CreateRingPool` in mi_venc.ko).
5. **Wrong VENC device / codec — STRUCTURAL, unfixable.**  i6e has two
   mi_venc devices: **dev0 = MHE**, the single unified H.264+H.265 core
   (`infinity6e.dtsi:157`, one `mhe-irq`), and **dev1 = JPE**, the JPEG
   engine (`dtsi:327`).  `/proc/mi_modules/mi_venc/mi_venc0` capability rows:
   dev0 `SupportRing=0 SupportImi=0`, dev1 `SupportRing=1 SupportImi=1`.
   The device is chosen internally by codec (`_MI_VENC_GetDevIdByCodecType`,
   no devid in `MI_VENC_CreateChn`) — both H.264 and H.265 land on dev0.
   **Ring input on i6e is effectively a JPEG-only capability.**
6. **Cross-check that the flag is real (the clincher):** on Maruko/i6c
   (`.233`), where our production `LINK_RING` SCL→VENC bind works, the same
   proc shows the H26x **dev0 `SupportRing=1`**.  Same driver architecture,
   same flag; i6c H26x has ring input, i6e MHE does not.
7. **Driver generations:** the SDK carries two mi_venc.ko vintages.  The
   2019 builds expose a `VENC_support_HWRING:bool` module param; the 2022
   firmware-based MHE driver ("chagall.bin" — what `.201` runs, build
   2022-06-07, param list matches exactly) removed it.  The vendor also
   ships **no** ring→VENC amigos config for the i6e demo family while other
   chip families get them — consistent with MHE lacking the input.

**Mechanism (disassembly-confirmed, unstripped SDK .ko with DWARF):**
`_MI_SYS_IMPL_BindChannelPort` computes `allowed = proposed & VPE-output-caps
& VENC-input-caps` and returns `0xA0092008` on an empty result.  The VPE
side (`_MI_SclRes_OnGetOutputPortBindCapability`) unconditionally advertises
`0x1d` toward VENC — never the blocker.  The VENC side
(`_MI_VENC_OnGetInputPortBindCapability`, mi_venc_impl.c) builds its mask
as: FRAME_BASE, `|= HW_RING` only if the device's
`MHAL_VENC_EXTRA_CAPABILITY_t.bExternalRingSupport` is set (the proc
`SupportRing` column) and the peer is VPE, `|= REALTIME` only if
`bImiSupport` (proc `SupportImi`; REALTIME into VENC is IMI/SRAM-backed).
The flags are queried from the mhal device table at `MI_VENC_IMPL_InitDev`.
On `.201` the MHE device reports 0/0 → input mask `0x01` → veto.  The
device-identity is double-confirmed by proc `MaxTaskCnt` 1/2 matching module
params `max_h26x_task=1` / `max_jpe_task=2` (dev0=H26x, dev1=JPE).  Further
bind-time preconditions (SetInputSourceConfig ring-mode coherence vs
`picHeight/ringLineCount`, one ring channel per device) sit *behind* the
capability gate and are all satisfied by our sequence.

**What would change the verdict:** a vendor mhal/chagall firmware release
that sets `bExternalRingSupport=1` for the MHE device (or a validated 2019
legacy-driver stack) — outside our control.  Until then `video0.lowDelay` on
Star6E always falls back to FRAMEBASE, by design.  REALTIME into VENC is
additionally IMI-gated and unsupported *everywhere* in the SDK samples
(REALTIME is used only for VIF→VPE), so HW_RING was always the only
realistic vehicle.

## 7. Risks

- **RING vs REALTIME on i6e VPE→VENC — RESOLVED NEGATIVE, root-caused
  (§6a/§6b).** Both rejected (`0xA0092008`) because the MHE H26x core's
  input port only advertises FRAME_BASE (`SupportRing=0` in the mhal device
  table; kernel mask trace `org input 0x01` vs VPE `org output 0x1d`).
  Cross-validated: i6c's H26x dev0 has `SupportRing=1` and our RING bind
  works there.  Ring on i6e exists only on the JPEG engine (dev1), which
  H26x channels can never land on.  Probe-and-fallback keeps this harmless;
  left in as flag-gated infrastructure pending a vendor driver/firmware that
  enables MHE ring input.
- **GetStream timing changes** under streaming input; the main loop's
  poll cadence may need adjustment.
- **Snapshot/dual loss under lowDelay** is a real feature trade until the
  second-VPE-port tap lands.
- **Teardown wedge** (unbounded RING flush) — mitigated by ordering +
  reinit soak; port the Maruko watchdog if needed.
