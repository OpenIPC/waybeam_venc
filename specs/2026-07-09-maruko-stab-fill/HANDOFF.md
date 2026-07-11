# Maruko stab-fill — HANDOFF (Phase F0a complete → F1/F2 decision)

> **RESOLVED 2026-07-09 — SHIPPED v0.37.0.** Option A was executed and landed
> the same day. The pivotal twist: §2's premise partially dissolved — Phase
> 5a's "the i6c VENC cannot be manually pushed" device result was a BufConf
> ABI artifact (BUFDATA_FRAME=1 on i6c, union at offset 24), and with the
> corrected layout the **direct VENC push encodes** — so the module-bind
> bridge in §3/§6 was never needed; the shipped shape is Star6E's
> (compose → push → VENC). See `plan.md` "OUTCOME" for the full record.
> This file is retained as the pre-implementation snapshot.

**Audience:** the next implementer (Fable) picking up deep troubleshooting +
implementation. Read `requirements.md` and `plan.md` (incl. "Phase F0a RESULTS")
first; this file is the current-state brief + the architectural decision.

**Goal (unchanged):** bring `video0.framing = stab-fill` to Maruko (i6c /
SSC378QE) for parity with Star6E — **full-FOV stabilization**: whole frame stays
visible, stabilized image floats on a moving black border (no crop-in). `stab`
(HW-crop window) already shipped v0.35.0.

---

## 1. What is proven / decided

- **The mechanism is SDK-proven.** i6c H.265 VENC is **bind-fed by design** (5a,
  merged #171). The canonical manual-feed (SDK `module_uvc.cpp` +
  `mid_scl_impl.cpp::PutStreamToSclInputPort`) is: CPU-compose a frame → inject
  into an **SCL input port** (`MI_SYS_ChnInputPortGetBuf/PutBuf`) → **`SCL→VENC`
  FRAME_BASE bind** with VENC in `E_MI_VENC_INPUT_MODE_NORMAL_FRMBASE` (= our
  `I6C_VENC_SRC_CONF_NORMAL` = 0). `ST_Sys_Bind` is byte-identical to our
  `g_mi_sys.fnBindChnPort2(0,src,dst,fps,fps,I6_SYS_LINK_FRAMEBASE,0)`.
- **Our binary reproduces the whole chain up to the bind** (F0a device run, `.233`):
  a 2nd SCL channel (dev 0 chn 1, no upstream bind) creates/starts/configures
  (IFC 1080×720)/enables — all `ret=0`. SCL manual-input is available to us.

## 2. The blocker F0a uncovered (the architectural constraint)

**One H26x VENC core, and it cannot mix RING + FRAME_BASE input across channels.**

- VENC channel ceiling: `MI_VENC_MAX_CHN_NUM_PER_DC = 3` → channels 0,1,2 only.
- `BindChnPort2(SCL(0,1,0)→VENC(0,2) FRAME_BASE)` while chn 0 is RING-fed
  ⇒ `0xA0092012` = **SYS / E_MI_ERR_BUSY**.
- 2nd H26x device (`MI_VENC_DEV_ID_H264_H265_1`) is **not backed by hardware**:
  `MI_VENC_CreateDev(1)` **blocks forever** on SSC378QE.

**Consequence:** a *sibling* frame-base channel cannot coexist with the live
RING encode. **BUT the real design has no such conflict** — in `stab-fill` the
composed frame *is* the main stream, so VENC dev 0 chn 0 must be **frame-base
from the start** (no RING leg). The detector still taps an **SCL output port**
(camera content) which is independent of the VENC feed mode. The probe just
can't demonstrate the encode without first tearing down the live RING.

## 3. Architectural choice (the design that follows)

`stab-fill` mode is a **different graph topology** from `off`/`zoom`/`stab`:

```
off / zoom / stab (shipped):
  VIF → ISP → SCL(0,0)  --RING-->  VENC(0,0)            [HW-fed, zero-copy]
                         └ port2 tap → (stab: reprogram SCL crop)

stab-fill (to build):
  VIF → ISP → SCL(0,0) → port2 tap ──→ detector (IVE NEON) → Kalman `acc`
                                                              │
  compose thread: read camera frame + shift by `acc` + black-fill borders
                  → MI_SYS_ChnInputPortPutBuf into SCL(0,1) input
  SCL(0,1)  --FRAME_BASE-->  VENC(0,0, NORMAL_FRMBASE)       [manual-fed]
```

Key points for the rewire:
- **VENC dev 0 chn 0 is frame-base**, fed ONLY by the compose bridge (SCL chn 1).
  There is NO camera→VENC RING leg in stab-fill mode.
- **Where does the camera frame for compose come from?** The compose needs the
  full-FOV NV12 to shift+border. Read it from an **SCL output port** (a full-res
  output port on SCL chn 0) via `ChnOutputPortGetBuf` — same family as the
  detector tap. (Detector taps port 2 already; the compose can share it or use a
  dedicated full-FOV output port. Decide during F1.)
- **The detector tap stays an SCL output** — unaffected by VENC being frame-base.
- **Compose is a real per-frame copy** (unlike `stab`'s crop-reprogram): full Y
  memcpy (shifted) + border fills. Star6E does this on a `SCHED_FIFO` blit
  thread — mirror that. The single-A7 CPU cost (detector ~8–17 ms + compose) at
  50 fps is the pivotal risk (R-F0) — measure early.

## 4. DECISION REQUIRED (was "Open decision" in requirements)

The clean/non-disruptive probe is exhausted. Two remaining unknowns: (a) the
frame-base bind succeeds once dev 0 is RING-free, (b) the compose+push CPU cost.
Pick one:

- **Option A — proceed straight to F1/F2 (RECOMMENDED).** Treat the encode as
  SDK-established (UVC proof + our topology stood up to the bind). Build the real
  frame-base rewire; measure (a)+(b) as the first thing that lights up during
  bring-up (they fall out for free — no throwaway code). Lowest total effort,
  no live-teardown risk.
- **Option B — destructive dev-0 ring-swap bench first.** Extend the probe: drain
  + disable main SCL output port 0, StopRecvPic main VENC chn 0, unbind
  `SCL(0,0,0)→VENC(0,0)` RING (frees dev 0), then bind
  `SCL(0,1,0)→VENC(0,2) FRAME_BASE`, inject, measure, then **kill the daemon**
  (don't restore). Gives (a)+(b) before writing F1, but briefly tears the live
  RING on the single H26x core — small wedge risk (i6c has no sysrq; recovery =
  `reboot -f`/power-cycle). Only worth it to de-risk before the larger rewire.
- **Option C — stop, ship stab-only.** Not warranted — nothing indicates
  infeasibility; the constraint is expected and the design absorbs it.

## 5. Code state (all on branch `feature/maruko-stab-fill-spec`, PR #172)

Committed with this handoff — the F0a bench is done and instructive; keep it:

- `src/maruko_stabfill_probe.c` — `maruko_stabfill_f0a_run()` (env
  `MARUKO_STABFILL_F0A`). Proven-working: SCL 2nd-channel stand-up + inject ABI
  (`PrbBufConf_t`/`PrbBufInfo_t`/`PrbFrame_t` = device-proven i6c MI_SYS input
  ABI). Reusable for Option B or as the F1 inject reference. Teardown follows the
  proven order (disable SCL out → StopRecvPic → unbind → destroy VENC/dev →
  stop/destroy SCL). `f0a_now_ms()` uses `CLOCK_THREAD_CPUTIME_ID` (CPU) +
  `CLOCK_MONOTONIC` (wall) for the compose timing.
- `include/maruko_stabfill_probe.h` — `maruko_stabfill_f0a_run` decl.
- `src/maruko_pipeline.c` — two hooks: (1) stash chn-0 VENC attr into
  `g_f0a_chn0_attr` at CreateChn time (env-gated); (2) call `maruko_stabfill_f0a_run`
  after `bind_maruko_pipeline` (post-dual block, ~line 3418) so the SCL device
  exists. Both env-gated → zero cost normally.
- `specs/.../plan.md` — "Phase F0a RESULTS" block (full numbers + decode).

## 6. Implementation map for F1/F2 (when greenlit)

- **Compose math (port verbatim):** `star6e_framing_stab.c:816-959`
  (`send_frame_to_venc_fill`) — shift in-bounds content rect, black-fill 4
  borders (Y=16, UV=128) via blit/fill. Only the *destination* changes (an SCL
  input buffer, not VENC input) + i6c fill/blit symbol names.
- **Inject primitive (working, in the probe):** `ChnInputPortGetBuf(SCL(0,1,0),
  BufConf{w,h,YUV420SP}) → fill → ChnInputPortPutBuf`. SDK ref
  `mid_scl_impl.cpp:667-708`.
- **SCL 2nd channel setup (working, in the probe):** `fnCreateChannel(0,1)` →
  `fnAdjustChannelRotation` → `fnStartChannel` → `fnSetPortConfig(0,1,0, i6c_scl_port{crop,out,YUV420SP,IFC(6)})`
  → `fnEnablePort(0,1,0)`. SDK ref `mid_scl_impl.cpp:121-249`.
- **Frame-base bind:** `g_mi_sys.fnBindChnPort2(0, &scl(0,1,0), &venc(0,0), fps,
  fps, I6_SYS_LINK_FRAMEBASE, 0)` — **after** dev 0 has no RING bind. SDK ref
  `module_uvc.cpp:774-925` (SetInputSourceConfig NORMAL_FRMBASE → SetMaxStreamCnt
  → Bind → StartRecvPic).
- **Main pipeline branch point:** `bind_maruko_pipeline()`
  (`maruko_pipeline.c:~2357`) currently always binds `SCL→VENC` RING at
  `:~2404` (`I6_SYS_LINK_RING`). Branch on `framing==stab-fill`: build the
  frame-base graph instead. Port config for VENC input source is set in
  `maruko_start_venc()` (`:~1990`, `input_mode = I6C_VENC_SRC_CONF_RING_DMA` →
  `_NORMAL` in stab-fill).
- **Module home:** add a fill-mode branch to `maruko_framing_stab.c` (mirror
  Star6E `g_stab_fill_mode`); reuse the shipped detector + `framing_kalman.{c,h}`
  + `stab_accuracy` (`framing_stab_accuracy.h`). `prepare()` already resolves
  accuracy + Kalman.
- **Teardown (R6, critical):** join the compose/detector thread BEFORE
  disabling/unbinding the bridge module + tap. The proven VENC/SCL teardown
  order is in `maruko_pipeline.c:4207-4298`. i6c has NO sysrq — a wedged flush =
  D-state = `reboot -f`.
- **Un-gate (F4):** `web/dashboard.html` disables the `stab-fill` enum option on
  Maruko (`optDisabled` "(N/A on Maruko)"); flip it + `make webui`.
  `venc_config.c` stab-fill preset comment. Follow the schema 7-touch checklist.

## 7. Device / deploy facts (`.233`, SSC378QE, i6c, musl)

- `root@192.168.2.233`. **No init-script auto-start** (`S95waybeam` moved to
  `/root`); launch manually: `waybeam` reads `/etc/waybeam.json`. Current bench
  config = IMX415 1080×720@50, H.265.
- **Backup exists:** `/root/waybeam.bak` (= factory `/usr/bin/waybeam`, md5
  `3d1c2268...`). Bench binaries pushed to **`/usr/bin/waybeam.new`** (do NOT
  overwrite `/usr/bin/waybeam`). Deploy via `cat`-pipe (scp flaky):
  `ssh root@.233 'cat > /usr/bin/waybeam.new && chmod +x ...' < out/maruko/waybeam`.
- **libmi_ive.so** = BSP uClibc blob (md5 `d608368e`) already in `/usr/lib`
  (required for `stab`; ships via builder#24 — STILL OPEN).
- **Never** `killall -9` / SIGKILL on i6c (MI_SYS zombie → power-cycle). SIGTERM
  only. F0a runs cleaned up on SIGTERM (no D-state observed).
- Build: `make build SOC_BUILD=maruko` (→ `out/maruko/waybeam`); full gate
  `make verify` (both backends). Cross-toolchain vendored.
- Hard SoC facts: **1 H26x VENC core**, **≤3 VENC channels/dev**, no 2nd H26x
  device.

## 8. Standing dependency

- **builder#24** (`libmi_ive.so` BSP blob) — runtime dep for ALL Maruko IVE
  stab work (incl. stab-fill's detector). Must merge to ship on firmware.
