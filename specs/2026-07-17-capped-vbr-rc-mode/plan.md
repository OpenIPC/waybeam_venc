# Plan: capped VBR RC mode

> **CLOSED — WITHDRAWN 2026-08-29.** See `requirements.md`: the per-frame
> caps this plan actuates were measured non-binding and removed in venc
> 0.72.0. The acceptance gate below ("no frame exceeds its
> `maxIBytes`/`maxPBytes`") is unmeetable on this hardware. Do not implement.

Companion to `requirements.md`. **Reconciled to PR #181 (2026-07-17):** the
per-frame ceiling mechanism is already implemented there
(`u32MaxISize`/`u32MaxPSize` + `SetRcPriority(FRAMEBITS_FIRST)`, both
backends, live + boot-applied). This plan now covers only the remaining
increment (QP window) and the shared validation, tracked **inside PR #181**.

## Design decisions (locked)

1. Encoder-enforced per-frame ceiling — **done in PR #181** via
   `FRAMEBITS_FIRST` (RC raises QP to fit; keeps every frame).
2. `VBR` (bounds `maxBitrate`) + the caps = capped VBR's two target bounds —
   **already achievable today** with PR #181 merged.
3. Star6E + Maruko parity — PR #181 already does both.

## Phase 0 — (obsolete) struct recovery

PR #181 modelled the needed RcParam fields (`u32MaxISize`/`u32MaxPSize`) and
added `MI_VENC_RcPriority_e` + `MI_VENC_SetRcPriority` (optional dlsym,
NULL-safe) for both SoCs. No separate struct-recovery phase is needed. The
one struct question that remains is the QP-window home (Phase 1).

## Phase 1 — expose the VBR QP window (the remaining increment)

1. Add `vbr_min_qp` / `vbr_max_qp` config fields (`video0`), camelCase
   aliases, load/save/render, defaults = full range (⇒ today's VBR).
2. Apply path: patch the ChnAttr rate struct
   (`i6_venc_rate_h26xvbr.maxQual`/`minQual`) via `SetChnAttr` — mirror
   `apply_bitrate` (`venc_api.c` rate switch). **Confirm against the SDK**
   whether the effective window is ChnAttr `maxQual`/`minQual` or RcParam
   per-mode `u32MinQp`/`u32MaxQp`, and the **polarity** (Qual vs Qp invert on
   some gens). If it is RcParam, fold it into PR #181's `apply_max_frame_size`
   path instead.
3. `MUT_LIVE` if the set path is live (bitrate already is). `make lint` after
   the config change and after the apply change (incremental).
4. `test_venc_config.c` cases (parse, defaults, serialize round-trip).

## Phase 2 — bench-validate (Star6E `192.168.1.13`, then Maruko)

Prefer `scripts/star6e_direct_deploy.sh cycle`. These extend PR #181's own
device test plan:

- **Ceiling holds:** scene with hard cuts; per-frame sizes via RTP sidecar
  (`frame_size_bytes`). Gate: no frame exceeds its `maxIBytes`/`maxPBytes`,
  across cuts and at IDR (IDR is an I-frame → QP-bumped to fit, not dropped).
- **IDR composition:** force scene-detect + API IDR; confirm it fits the I
  cap and the stream recovers; reconcile with `idr_rate_limit.h`.
- **No fps loss:** run 1080p60 and 1080p120; the `FRAMEBITS_FIRST` QP-bump
  must not blow the ~8.3 ms budget at 120fps. Document the envelope.
- **No limit cycle:** frame sizes not pinned at the cap every frame (⇒ cap
  below VBR's natural size at `maxBitrate`; raise it / widen `statTime`).
- **QP window (Phase 1):** `FRAMEBITS_FIRST` + `maxQual` floor → no visible
  quality craters on complex frames; easy scenes dip below `maxBitrate`.

## Phase 3 — waybeam-link retires its per-frame cap

Once the encoder ceiling is proven in production, remove the link-side
maxI/maxP enforcement (separate repo). Keep link's bitrate/QP setpoint
control. Do NOT run both caps in production — double-clamp masks encoder-side
bugs.

## Verify

`make verify` (both backends) + targeted deploy tests per `AGENTS.md`.
`VERSION` + `HISTORY.md` bump land with whatever PR ships the QP window (or
with PR #181 if folded in).

## SDK findings (fill in when the full SDK is mounted)

- [ ] QP-window home: ChnAttr `maxQual`/`minQual` vs RcParam `u32MinQp`/
      `u32MaxQp` — and polarity: …
- [ ] `FRAMEBITS_FIRST` behaviour at 120fps (QP-bump cost / fps envelope): …
- [ ] IDR vs `maxIBytes` composition on device: …
