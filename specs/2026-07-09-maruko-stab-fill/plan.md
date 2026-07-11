# Plan — Maruko `stab-fill` via module-bind compose

See `requirements.md` for goal, background (Phase 5a), and reuse. This plan is
gated on **Phase F0** (the one remaining unknown: does compose→module→VENC
frame-base encode in *our* pipeline, and at what CPU cost on the single A7).

## Reference points

- Star6E compose: `star6e_framing_stab.c:816-959` (`send_frame_to_venc_fill`) —
  shift the in-bounds content rect, black-fill the four border rects (Y=16,
  UV=128), via `BufBlitPa`/`BufFillPa` on the destination buffer. The *math*
  ports verbatim; only the destination (a module input buffer, not VENC) and
  the fill/blit calls (i6c symbol names) change.
- SDK inject→bind: `.../interface/uvc/module_uvc.cpp` — `MI_SYS_ChnInputPortGetBuf/
  PutBuf` into SCL input; `stBindInfo[1]` = `SCL→VENC` `E_MI_SYS_BIND_TYPE_FRAME_BASE`;
  VENC `E_MI_VENC_INPUT_MODE_NORMAL_FRMBASE`.
- SDK SCL manual input: `.../mid/maruko_impl/scl/mid_scl_impl.cpp:662,703`.
- Maruko VENC create + current RING bind: `maruko_pipeline.c:~2000` (create dev/chn,
  `SetInputSourceConfig(RING_DMA)`), RING bind `SCL→VENC` at `:~2391`.
- i6c MI_SYS input ABI (device-proven): `maruko_framing_stab.c` StabFrame_t /
  StabBufInfo_t; conf layout in `maruko_stabfill_probe.c` (PrbBufConf_t).

## Phase F0 — DE-RISKING BENCH (go/no-go, do FIRST, like 5a)

**Question:** in our pipeline, does a composed frame injected into a bridge
module and FRAME_BASE-bound to VENC actually encode, and is the single-core CPU
compose cost acceptable at 50 fps?

- Extend the existing env-gated probe harness (`maruko_stabfill_probe.c` style).
- **F0a (primary, SCL-inject):** create a 2nd SCL channel; set its input to
  manual (pushable); FRAME_BASE-bind `SCL2→VENC(chnN, NORMAL_FRMBASE)`; push
  ~30 hand-composed gray+border frames into SCL2 input; confirm VENC emits
  (`Query.curPacks>0` / `GetStream`). Measure per-frame compose+push CPU.
- **F0b (fallback, DIVP):** only if F0a's compose cost pegs the core — bench
  `SCL→DIVP→VENC` with DIVP doing the offset-blit-into-black-canvas; confirm
  DIVP supports it and that it encodes.
- **Decision gate:** pick A or B (or, if neither encodes in our pipeline,
  document stab-fill as i6c-infeasible and STOP — ship stab-only). Record
  numbers here, like the Phase 1 / 5a RESULTS blocks.

### Phase F0a RESULTS (device `.233`, SSC378QE, 2026-07-09)

Env-gated `MARUKO_STABFILL_F0A` bench (`maruko_stabfill_probe.c
:maruko_stabfill_f0a_run`, hooked post-`bind_maruko_pipeline`). Three iterations:

1. **SCL-inject topology stands up in our binary.** A 2nd SCL channel (dev 0,
   chn 1, no upstream bind) created + started + output-port(IFC 1080×720)
   configured + enabled — **all `ret=0`**. So SCL manual-input (the
   `PutStreamToSclInputPort` primitive) is available to us.
2. **VENC channel ceiling = 3.** `MI_VENC_MAX_CHN_NUM_PER_DC = 3` → valid H26x
   channels are **0,1,2 only**; CreateChn(0,6) rejected (SDK returns raw `31`,
   not an `MI_DEF_ERR`). Retest at chn 2 → `CreateChn=0`. *(Recontextualises 5a:
   its chn 4/5 were also out-of-range, so 5a's device signal was weaker than a
   clean "push doesn't encode" — the SDK-source conclusion still stands.)*
3. **FRAME_BASE can't coexist with the live RING encode on the single H26x
   device.** `BindChnPort2(SCL(0,1,0)→VENC(0,2) FRAME_BASE)` = `0xA0092012` =
   **SYS / E_MI_ERR_BUSY**. The main encode (dev 0 chn 0) is RING-fed; a
   frame-base bind on another channel of the *same* VENC device is refused.
4. **The 2nd H26x device is not backed by hardware.** Moving the bridge to
   `MI_VENC_DEV_ID_H264_H265_1` (dev 1): `MI_VENC_CreateDev(1)` **blocks
   forever** (printf after the call never fires across 28 s while other threads
   keep logging). SSC378QE has a single H26x core.

**Net:** the SDK already proves the compose→SCL-inject→`SCL→VENC` FRAME_BASE
topology encodes H.265 (UVC path; `ST_Sys_Bind` is byte-identical to our bind).
Our binary reproduces the whole chain **up to** the frame-base bind; the only
blocker is that a frame-base VENC can't run **alongside** the live RING encode on
the one H26x device. **This is not a problem for the real design** — in
`stab-fill` the composed frame *is* the main encode, so VENC dev 0 chn 0 is
frame-base from the start and there is no competing RING leg. The sibling-probe
simply can't demonstrate that without tearing the live RING down first.

**Still unproven in-binary:** (a) the frame-base bind actually succeeds once dev
0 is RING-free, and (b) the single-A7 compose+push CPU cost at 50 fps. Both fall
out naturally from the F2 rewire (build VENC frame-base from the start) and can
be measured at F5 — or via a **destructive** dev-0 ring-swap bench first
(unbind the live RING → bind frame-base → inject → measure → daemon killed).
Clean, non-disruptive probing is exhausted. **DECISION PENDING** (see
requirements "Open decision").

## OUTCOME (Option A executed, 2026-07-09 — SHIPPED v0.37.0)

Option A (straight to F1/F2) was chosen and **succeeded the same day**, with a
finding that rewrites the Background:

**Phase 5a's device result was an ABI ARTIFACT — the i6c H.265 VENC DOES
encode direct manual pushes.** Three silent i6e→i6c ABI divergences broke both
the 5a and F0a probes' `MI_SYS_BufConf_t`:
1. `E_MI_SYS_BUFDATA_FRAME` is **1** on i6c (`RAW = 0`) — the probes pushed
   RAW-typed buffers.
2. i6c `BufConf` carries `bDirectBuf` + `bCrcCheck` between `u64TargetPts` and
   the config union → the union sits at offset **24**, not 16 (Star6E shape) —
   the SDK read w=0/h=0 and returned degenerate zero buffers with `ret=0`.
3. i6c `BufFrameConfig` has NO embedded `FrameBufExtraConfig`.

With the corrected layout, `ChnInputPortGetBuf/PutBuf` into a `NORMAL_FRMBASE`
VENC(0,0) input encodes at the full 50 fps (`FinishCnt` tracks pushes 1:1,
DropCnt 0). Two more required pieces:
- `MI_SYS_SetChnInputPortFrc(USERINJECT, fps/fps)` on the unbound input port —
  without it the port sits at 0/0 FRC (harmless for VENC, fatal for SCL — see
  below).
- `MI_SYS_BufBlitPa`/`BufFillPa` exist in i6c libmi_sys (leading `u16SocId`
  arg) — the compose is HW-blit like Star6E, ~3.0 ms/frame wall.

**The SCL-inject bridge (F0a topology) was implemented first and abandoned:**
SCL chn 1 accepts injected input (queues in `UsrInjectQ`) but its scheduler
never dispatches the tasks (`workingTask_cnt=0` forever) regardless of FRC,
input crop, or SDK bring-up ordering. Academic now — the direct push obviates
the bridge entirely.

**Shipped shape (v0.37.0):** identical to Star6E's — port0 RAW+unbound →
drain/detect/Kalman → HW-blit compose → push to VENC. Measured on `.233`
(IMX415 1080×720@50, `stab_accuracy=low`): detect 5.2 ms + compose 3.0 ms =
**5.8 ms/frame thread CPU** (~29% of the A7). Visual confirmed (floating image
on black border, live on the dev-host viewer); 5/5 SIGTERM teardown cycles
clean, dmesg clean. F4 un-gate done. `record.mode=dual` refused under
stab-fill (RING chn 1 can't coexist with frame-base chn 0 — the one F0a truth
that still stands).

### Reinit-switch hardening (post-implementation, same day)

Adversarial resolution/sensor-mode/preset switching (~40 API reinits) exposed
three issues, all fixed pre-merge:

1. **Teardown wedge (BOTH stab presets, root cause pre-existing).** A
   user-drained SCL port whose consumer thread is joined before the port stops
   producing pins SCL/ISP working tasks → the ISP→SCL REALTIME unbind's
   unbounded kernel flush wedges in D-state (`MI_SYS_IMPL_FlushRealTimeOutputBuf`)
   → zombie or hardware-watchdog reset.  Reproduced from stab-fill (port0),
   plain stab (the port-2 tap — meaning v0.35.0 was exposed), and the factory
   binary (pre-existing SDK race, first logged 2026-07-03).  Fix: two-phase
   stop — drain-only thread keeps consuming through the port disable, joined
   after (`maruko_framing_stab_finish_stop`).  Cut incidence ~10×.
2. **Residual race → teardown watchdog.** One wedge in ~20 switches survived
   the drain (SCL working task pinned with the FIFO empty).  CONTROL TEST:
   `framing=off` wedged on its FIRST size-change reinit — the residual race is
   GENERIC and pre-existing (no stab code in the path; the factory binary and
   the 2026-07-03 hangs match).  Userspace ordering cannot close it, so the
   roadmap's teardown watchdog is now shipped: 12 s deadline at teardown entry
   → `reboot(RB_AUTOBOOT)` — bounded, logged, self-recovering (~45 s outage)
   instead of open-ended limbo.  Observed working repeatedly during the
   barrage (device self-recovered every residual event, no manual
   intervention).  Net: reinit switching is now strictly MORE reliable than
   master in every framing mode.
3. **Live fps change stalled the fill VENC dead.** `maruko_apply_fps` used an
   SCL→VENC RING unbind/rebind as the fps divider — illegal on the frame-base
   manual-fed VENC.  Fill mode now updates the VENC input's USERINJECT FRC
   (src:dst) instead; live fps 50↔30 device-verified.

Observed behavior note: fill-mode stabilization sensitivity scales with the
ENCODE resolution — the detector measures on the encode-domain frame (Star6E
design parity), so at small sizes (e.g. 640×360 from a 2952-wide sensor, one
encode pixel ≈ 4.6 sensor pixels) small shakes quantize to zero measurement.
Plain `stab` does not have this property: its tap is a 1:1 SCL-INPUT-domain
crop, full-resolution motion sensing at any encode size.  Documented as
inherent; prefer `stab` for very small encodes.

## If F0 passes — the port

### F1 — compose helpers (port Star6E's fill/blit to i6c)
- Add fill-rect (`BufFillPa`) + blit-rect (`BufBlitPa`) helpers for the i6c
  MI_SYS symbols (dlsym; the stab module already loads the sibling MI_SYS
  symbols). Y=16 / UV=128 borders, NV12 even-offset clamping — mirror
  `star6e_framing_stab.c` fill/blit exactly.
- Reuse `stab_crop_pct` as the max-shift / border budget (already the semantics
  under stab-fill in `venc_config.c`).

### F2 — pipeline rewire (stab-fill mode only)
- When `framing==stab-fill`: build the bridge chosen in F0 —
  - (A) a 2nd SCL channel with manual input + `SCL2→VENC` frame-base bind, VENC
    `NORMAL_FRMBASE`; the main camera→SCL keeps feeding the detector tap; OR
  - (B) insert DIVP between SCL and VENC.
- `off`/`zoom`/`stab` keep the existing camera→SCL→VENC(RING) path untouched
  (branch on the framing preset at graph-configure time).

### F3 — wire into `maruko_framing_stab.c` (fill-mode branch)
- Add `g_fill_mode` (mirror Star6E). `prepare()` already resolves the accuracy
  level + Kalman; in fill mode the detector still taps SCL port 2, but the EMIT
  path becomes: read the full SCL frame → compose (shift by Kalman `acc`,
  black-fill borders) into the bridge-module input buffer → PutBuf.
- The compose may need a dedicated thread (Star6E uses a `SCHED_FIFO` blit
  thread) so it doesn't stall the detector/drain loop — decide from F0's timing.
- Teardown: keep the R6 rule — join the compose/detector thread BEFORE
  unbinding/disabling the bridge module and the tap (MMU-storm → watchdog).

### F4 — un-gate stab-fill (schema + WebUI)
- `venc_api.c` `field_supported_for_backend`: currently no maruko gate on
  `framing` values, but `web/dashboard.html` still disables the `stab-fill`
  enum option on Maruko (`optDisabled`) with an "(N/A on Maruko)" label — flip
  those, update the framing tooltip, regen `venc_webui.c` via `make webui`.
- `venc_config.c` stab-fill preset comment → note the Maruko module-bind path.

### F5 — device verify (.233)
- framing=stab-fill applies; full-FOV floating-on-black confirmed on the RTP
  view; pan + `pause_stab` glide-home work; ≥10 stop/start cycles clean, 0 MMU
  resets; record fps + CPU vs `stab`.

## Risks

- **R-F0 (pivotal):** F0 may show the compose+bind path encodes but the
  single-core CPU cost (detector ~8–17ms + compose + copy) blows the 20ms
  budget at 50 fps → stab-fill only viable at a lower fps or with DIVP HW
  compose. F0 measures this before any port.
- **R-F1 (topology):** the 2nd-SCL-channel bridge (A) means VENC is fed by SCL2
  not SCL0; the detector tap + the camera path must coexist with SCL2 on the
  same SCL device — verify SCL supports the extra channel + manual input while
  chn0 runs (an early F0a check).
- **R-F2 (DIVP unknown):** DIVP offset-blit-into-canvas is unverified on i6c;
  (B) only viable if F0b confirms it.
- **R-F3 (teardown):** the fill path adds a bridge module + compose thread to
  the join-before-disable ordering; more MMU-storm surface than `stab`.
- **R-F4 (zero-copy loss):** stab-fill is inherently a copy (compose), unlike
  `stab`'s crop-reprogram — a real per-frame cost, acceptable by design.

## Effort

F0 ~1 focused bench session. If it passes, F1–F5 ~ multi-day (larger than
`stab` was — new bridge + compose + rewire). If F0 fails on cost/feasibility,
stop and ship stab-only with the finding recorded.
