# Maruko (Infinity6C) `video0.framing = stab-fill` via module-bind compose

Status: **SHIPPED (v0.37.0, 2026-07-09)** — via **direct VENC push**, NOT the
module-bind bridge this spec scoped: Phase 5a's "no direct push" device result
turned out to be a BufConf ABI artifact (see plan.md "OUTCOME"). All acceptance
criteria met on `.233` (visual + 5-cycle teardown soak + 5.8 ms/frame CPU).
The Background below is retained as written for the record; read it together
with the OUTCOME correction.
Date: 2026-07-09
Device: `root@192.168.2.233` (i6c / SSC378QE, OpenIPC, musl)
Supersedes: the "Phase 5" stab-fill sketch in
`specs/2026-07-08-maruko-stab/plan.md` (that plan assumed Star6E's push-to-VENC
path; Phase 5a proved it inapplicable on i6c).

## Goal

Bring `video0.framing = stab-fill` to the Maruko backend for parity with
Star6E: **full-FOV stabilization** — the whole frame stays visible and the
stabilized image floats on a moving black border (no crop/zoom-in), vs `stab`
which HW-crops a window (already shipped on Maruko, v0.35.0).

## Background — why this needs a new mechanism (Phase 5a, DONE)

Star6E's stab-fill composes each frame in software and pushes it straight into
an unbound **VENC** input port (`MI_SYS_ChnInputPortGetBuf/PutBuf`). **That does
not work on the i6c H.265 VENC.** Phase 5a (`src/maruko_stabfill_probe.c`,
merged PR #171) proved on `.233`: a VENC channel in `NORMAL`/`HW_SYNC`
*accepts* pushed frames (GetBuf/PutBuf succeed) but emits **zero encoded
packets**; `StartRecvPicEx(-1)` is rejected. The main RING-fed channel encodes
fine concurrently, isolating the failure to the push path.

The Maruko SDK (`/home/snokvist/dev/Maruko`) settles the architecture:
- `mid_venc_impl.cpp` (H.264/H.265) **always** feeds VENC via `BindChnPort2`
  (FRAME_BASE / REALTIME / HW_RING) — never a direct push.
- `module_uvc.cpp` (the canonical manual-feed for H.265) injects frames into
  **SCL**, then binds `SCL → VENC` **frame-base** with VENC in
  `E_MI_VENC_INPUT_MODE_NORMAL_FRMBASE`.
- Only the separate JPEG device (`mid_jpeg_impl.cpp`) accepts a direct push.

**Conclusion: the i6c H.265 VENC is bind-fed by design.** stab-fill must
**compose the shifted+bordered frame, inject it into a processing module (SCL
manual input, or DIVP HW compose), and FRAME_BASE-bind that module → VENC.**
This is proven to encode H.265 in the SDK (UVC).

## Reuse (already shipped — do NOT rebuild)

- Motion detector (`MI_IVE_Shift_Detector`) on the SCL port-2 center tap, and
  the shared Kalman control law (`framing_kalman.{c,h}`) — identical to `stab`.
- `video0.stab_accuracy` detector level (`framing_stab_accuracy.h`, v0.36.0) —
  stab-fill inherits it via the same resolve in `prepare()`.
- The `maruko_framing_stab.c` module + registry + `pause_stab` plumbing — add a
  fill-mode branch (mirror Star6E's `g_stab_fill_mode`), don't fork a module.
- The compose math (shift content + black-fill Y=16/UV=128 borders) ports from
  `star6e_framing_stab.c:816-959` (`send_frame_to_venc_fill`) — only the
  *inject target* changes (module input, not VENC input).

## In scope

- `framing = stab-fill` on Maruko: detector + Kalman (shared) → per-frame
  compose → module-inject → FRAME_BASE-bind → VENC.
- The pipeline rewire for stab-fill mode only (`SCL→VENC` RING → frame-base, or
  a DIVP hop; VENC `NORMAL_FRMBASE`). `stab`/`off`/`zoom` paths unchanged.
- Un-gate `stab-fill` in `venc_api.c` supported-for-backend + dashboard.

## Out of scope / non-goals

- Hardware-accelerating the detector (impossible; NEON software — see the stab
  cost model in `specs/2026-07-08-maruko-stab/requirements.md`).
- Gyro/IMU fusion.
- Changing the shipped `stab` (HW-crop) path.

## Acceptance criteria

1. `video0.framing = stab-fill` validates and applies on Maruko (no 501).
2. Encoded output shows the full FOV floating on a black border that moves
   opposite to camera shake; pan/zoom and `pause_stab` (glide-home) still work.
3. Detector runs at sensor fps (or a chosen decimation) with no MI_SYS
   MMU-callback storm / watchdog reset; encoder holds an acceptable fps
   (document the cost; single-core compose stacks on the ~8–17ms detector).
4. Teardown clean across ≥10 start/stop cycles (no zombie, no reboot).
5. WebUI no longer greys out `stab-fill` on Maruko.

## Open decision (resolved by Phase F0 bench)

**Which module bridges the composed frame to VENC?**
- **(A) SCL manual-input** (UVC-style): CPU-compose → push to a 2nd SCL
  channel's input → `SCL→VENC` frame-base. SDK-proven; CPU compose (like
  Star6E); needs a 2nd SCL channel.
- **(B) DIVP HW-compose**: `SCL→DIVP` bind; DIVP blits the shifted frame into a
  black canvas → `DIVP→VENC` frame-base. Offloads the compose from the single
  A7 — IF i6c DIVP supports offset-blit-into-canvas (unverified). i6c has DIVP
  (`I6C_SYS_MOD_DIVP`, `MI_DIVP_SetChnAttr`).

F0 benches whichever is simplest first (A), escalates to B if the single-core
CPU compose cost is unacceptable.
