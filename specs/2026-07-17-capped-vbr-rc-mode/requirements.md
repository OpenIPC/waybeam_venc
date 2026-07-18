# Capped VBR RC mode (bounded bitrate + deterministic per-frame size)

**Status: SPEC — reconciled to PR #181 (2026-07-17).** Replaces the existing
`vbr` mode with a link-matched rate-control mode.

> **Update — mechanism corrected by PR #181** ("Add live MaxISize/MaxPSize
> frame-size caps with RC priority", branch
> `claude/waybeam-full-frame-shm-fec-kuf9p8`). The per-frame ceiling is **not**
> the SuperFrame/`REENCODE` path originally hypothesised below — it is
> `u32MaxISize`/`u32MaxPSize` (bytes) in the RcParam per-mode union +
> `MI_VENC_SetRcPriority(FRAMEBITS_FIRST)` (RC raises QP to fit the cap).
> PR #181 already ships this across CBR/VBR/AVBR, both backends, live +
> boot-applied. **Net: `VBR` + `maxIBytes`/`maxPBytes` already delivers the
> two target bounds.** The remaining increment for capped VBR is exposing the
> **QP window** (`minQual`/`maxQual`); see "Remaining work". Implementation is
> tracked in PR #181, not a fresh branch.

## Motivation

waybeam-link runs the encoder in **CBR** and enforces **per-frame maxI/maxP
size caps on the link side**. Two limits of that arrangement:

- CBR fills the channel to the target even on easy scenes (spends airtime /
  loss exposure on low-value bits).
- A link-side per-frame cap can only react *after* an oversized frame exists
  (drop/fragment) — it cannot shrink the frame, so a scene cut still emits a
  burst of packets before the cap bites.

We want **one** encoder mode that gives both properties the link needs:

1. **Bounded upper bitrate** — never exceed a ceiling the channel can carry.
2. **Deterministic per-frame size** — a hardware-guaranteed maxI / maxP so a
   single frame can never spew a correlated packet burst onto a lossy link.

This replaces `vbr` (little-used today) rather than adding a fourth mode —
we do not want to maintain two VBR variants.

## Locked decisions (from design review)

1. **Enforce the per-frame ceiling in the encoder** — not on the link side.
   The encoder emits a frame that already fits; waybeam-link can retire its
   own per-frame cap logic. *Realised in PR #181 via `u32MaxISize`/
   `u32MaxPSize` + `SetRcPriority(FRAMEBITS_FIRST)`.*
2. **Overflow behaviour = raise QP to fit, keep the frame** (not drop). Every
   frame is kept (preserves the P-reference chain and SVC-T layering); an
   oversized frame just takes a one-frame quality dip. *`FRAMEBITS_FIRST`
   delivers exactly this — the RC bumps QP rather than dropping.*
3. **Replace `vbr`** — `rc_mode = "vbr"` becomes capped VBR. No separate mode
   string. (Old configs keep parsing; new fields default per "Migration".)
4. **Star6E + Maruko parity** (per `AGENTS.md` dual-backend policy). PR #181
   already implements the per-frame caps on both; the QP-window increment
   follows the same dual-backend pattern.

## What the mode is

Three bounds, one mode:

| Bound | Mechanism | Where / status |
|---|---|---|
| Upper bitrate | VBR `maxBitrate` | ChnAttr rate struct (**already wired**, live via `video0.bitrate`) |
| Per-frame size | `u32MaxISize`/`u32MaxPSize` (bytes) + `SetRcPriority(FRAMEBITS_FIRST)` | RcParam per-mode union (**shipped in PR #181** — `maxIBytes`/`maxPBytes`) |
| Quality window | QP floor/ceiling (`minQual`/`maxQual`, or RcParam min/max Qp) | ChnAttr rate struct or RcParam (**remaining work** — confirm home + polarity vs SDK) |

## Remaining work (post PR #181)

1. **Expose the VBR QP window** (`minQual`/`maxQual`). With `FRAMEBITS_FIRST`,
   the RC bumps QP to hit the byte cap; without a floor/ceiling that means
   quality craters on complex frames and wasted bits on easy ones. The window
   is the natural partner to the size caps. Likely a `SetChnAttr` patch on
   `i6_venc_rate_h26xvbr.maxQual`/`minQual` (same pattern as `apply_bitrate`),
   distinct from PR #181's `SetRcParam` path — confirm the effective home
   (ChnAttr vs RcParam `u32MinQp`/`u32MaxQp`) and polarity against the SDK.
2. **Consolidate/document** the "VBR + QP window + size caps" profile as the
   recommended capped-VBR config; decide whether to retire the little-used
   plain modes.

## What PR #181 established (mechanism, both backends)

- **Per-frame caps:** `u32MaxISize` / `u32MaxPSize` (bytes) set in the RcParam
  per-mode union member matching the active rate mode
  (`stParamH265{Cbr,Vbr,Avbr}` / `stParamH264{Cbr,VBR,Avbr}`) via
  `GetRcParam` → patch → `SetRcParam`.
- **Hard-ceiling switch:** `MI_VENC_SetRcPriority(FRAMEBITS_FIRST)` when either
  cap > 0 (the RC then honours the byte cap over the bitrate target by raising
  QP); `BITRATE_FIRST` when both return to 0. `MI_VENC_RcPriority_e` +
  `fnSetRcPriority` added to both backends as an **optional dlsym** symbol
  (NULL-safe), arity differing (Star6E 2-arg, Maruko 3-arg).
- **Config / API:** `video0.maxIBytes` / `video0.maxPBytes` (default 0 =
  unlimited), `MUT_LIVE`, applied atomically via `apply_max_frame_size` and
  at boot. An IDR is requested (rate-limited) on change.

This resolves the struct-layout and mechanism unknowns the original draft
listed as blockers — no SuperFrame modelling is needed.

### Remaining SDK question (only one)

**QP-window home + polarity.** Is the effective quality window the ChnAttr
VBR rate struct's `maxQual`/`minQual` (`i6_venc_rate_h26xvbr`,
`sigmastar_types.h:569-577`), or RcParam per-mode `u32MinQp`/`u32MaxQp`? And
which bound is the floor vs ceiling (Qual vs Qp conventions invert on some
gens)? Confirm against the SDK before wiring Phase 1.

## Config surface

Already shipped (PR #181), `video0`:

- `maxIBytes` / `maxPBytes` — per-frame I/P byte caps. `MUT_LIVE`, 0 =
  unlimited.

To add (QP-window increment):

- `vbr_min_qp` / `vbr_max_qp` (names TBD to match `venc_config` conventions) —
  the quality window. Default = full range ⇒ today's VBR. Live via the same
  reconfigure pattern. Add `test_venc_config.c` cases.

## Migration (`vbr` → capped VBR)

- The "mode" is a *profile*, not a new `rc_mode` string: run `rc_mode="vbr"`
  (bounds `maxBitrate`) with `maxIBytes`/`maxPBytes` set (deterministic
  per-frame size) and, once added, the QP window.
- All-defaults (`maxIBytes=maxPBytes=0`, full QP range) reproduces plain VBR,
  so existing configs are unaffected.
- Document the profile in `HISTORY.md` under the version that ships the QP
  window / consolidation.

## Open questions / risks

1. **QP-window home + polarity** (see above) — the only remaining SDK
   confirm; blocks the Phase 1 wiring, not the shipped caps.
2. **`FRAMEBITS_FIRST` QP-bump cost at high fps** — hitting a hard byte cap by
   steering QP within a single encode should be cheaper than an explicit
   re-encode pass, but the internal cost is an SDK detail. Measure at imx335
   **120fps** (~8.3 ms budget); characterise the fps/latency envelope.
3. **IDR interaction** — an IDR is an I-frame, so it is subject to
   `maxIBytes`. Confirm scene-detect and resilience-preset IDRs get QP-bumped
   to fit (not dropped); reconcile with `idr_rate_limit.h`.
4. **Resilience-preset ownership** — presets drive intra-refresh + SVC-T + GOP
   under a CBR assumption. Decide whether the capped-VBR profile composes with
   the presets or is selected independently.
5. **Cap ↔ VBR-loop limit cycle** — a cap set well below the VBR loop's
   natural per-frame size at `maxBitrate` pins every frame at the cap (quality
   breathing). Keep caps ≥ that natural size; widen `statTime` to soften.
6. **QP-window necessity** — without a `maxQual` floor, `FRAMEBITS_FIRST` can
   crater quality on complex frames; without a `minQual` ceiling it wastes
   bits on easy ones. This is the motivation for the Phase 1 increment.

## Verified fact sources

- PR #181 diff (`u32MaxISize`/`u32MaxPSize`, `MI_VENC_RcPriority_e`,
  `apply_max_frame_size`, dual-backend `fnSetRcPriority`)
- `include/sigmastar_types.h:527-616` (VBR/QP/CBR rate structs, `maxQual`/
  `minQual`), `514-525` / `895-908` (rate-mode enums, i6e + i6c)
- `src/venc_api.c:2891-2932` (VBR wired for maxBitrate+gop; live SetChnAttr)
- `src/venc_config.c` + `include/venc_config.h:103` (`rc_mode`, `max_*_bytes`)
