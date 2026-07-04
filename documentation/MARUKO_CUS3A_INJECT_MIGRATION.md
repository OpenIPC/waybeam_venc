# Maruko (I6C) AE: migrate `EnableUserspace3A` → Cus3A inject-mode

**Branch:** `feature/maruko-cus3a-apply-cost`
**Date:** 2026-07-04
**Status:** env-gated Option-B prototype implemented + device-validated.
**CPU thesis PROVEN** (70→38.9% @1080p100, full-rate). **Option B REFUTED** for
image quality — see "DEVICE VALIDATION" at the bottom; the shippable path is
Option A (native vendor AE+AWB in inject-mode). Default behavior unchanged (gate off).
**Prize:** full-rate, vendor-quality AE at **majestic CPU**.

## The finding (corrected mechanism)

The residual CPU gap after AE-throttle (waybeam 52.8% vs majestic 42.9% @1080p100)
and the un-throttled +13pt `IspMidThreadWq` both trace to **one call**:

- waybeam enables AE via **`MI_ISP_EnableUserspace3A`**, which internally calls
  **`MI_ISP_RegisterIspApiAgent`** — the SDK's *"API agent for SigmaStar user
  space 3A library."* This **relocates the entire SigmaStar IQ/3A mid-layer into
  waybeam's process** and tunnels the ISP IQ API through it. Consequence: every
  per-frame `MI_ISP_CUS3A_SetAeParam` forces a full in-process IQ/CMDQ mid-pass
  → `IspMidThreadWq` ~13% + heavy `isp0_P0_MAIN` at 100 Hz.
- majestic enables AE via **`MI_ISP_CUS3A_Enable` + `MI_ISP_CUS3A_InjectModeEnable`**
  — plain ioctl messages, **no agent registered**. `SetAeParam` becomes a
  lightweight delta injected into the ISP *server's* already-running pipeline
  → `IspMidThreadWq` ≈ 0, `isp0` ≈ 7%.

**The apply call is byte-identical in both modes** (`SetAeParam` = one ioctl,
API-id `0x2e06`). The cost was never the apply — it is whether the API agent is
registered. (SDK evidence: disassembly of `libmi_isp.a userspace_3a_obj_user.o`
shows `EnableUserspace3A` → `CUS3A_Init` + `EnableUserspaceAE/AWB/AF` +
`RegisterIspApiAgent`; `libcus3a.a` shows `CUS3A_SetRunMode` →
`MI_ISP_CUS3A_InjectModeEnable`; header `mi_isp_cus3a_api.h:125-129`.)

This also **overturns two stale assumptions** in our notes:
- *"`EnableUserspace3A` is mandatory ≥60fps (FIFO stalls without it)"* — **false.**
  majestic runs 1080p100 with no `EnableUserspace3A` at all. It was a
  waybeam-specific workaround, not a hardware requirement.
- *"Fixing throttle AE quality (P1–P6) is the only path to both CPU + quality;
  native ISP cost is stuck ~60–66%."* — **false.** Inject-mode gets both.

Proven twice: majestic (I6C, inject-mode) and our own Star6E backend (I6E,
`CUS3A_Enable`-only, no `EnableUserspace3A`) both run vendor-framework AE without
the agent.

## Device de-risk (already confirmed on .12)

All required symbols are present and dlsym-able in the shipped `/usr/lib/libmi_isp.so`:
`MI_ISP_CUS3A_InjectModeEnable`, `MI_ISP_CUS3A_Enable`, `MI_ISP_CUS3A_SetAeParam`.
`libcus3a.so` is also present (needed only for the heavier Option A). majestic
references `InjectModeEnable` at runtime.

## The change — "Option B" (minimal; keeps our P1/P4 controller)

Because the apply is identical, **we keep the entire P1/P4 IIR AE loop unchanged**
(`maruko_cus3a.c` control law + per-frame `SetAeParam`). We only change the
*enable plumbing* so no API agent is registered:

| # | Where | Change |
|---|---|---|
| 1 | `maruko_pipeline.c:201-231` `maruko_enable_cus3a()` | **Remove** the `MI_ISP_EnableUserspace3A(0,0)` call (lines ~224-227). **Add** `MI_ISP_CUS3A_InjectModeEnable(0,0,&{.bInject3A=1})` after `MI_ISP_CUS3A_Enable`. Resolve the new symbol via dlsym. |
| 2 | `maruko_cus3a.c:600-633` `maruko_cus3a_install_noop_adaptor()` | **Remove / skip.** The no-op AE adaptor existed to suppress the native AE under the userspace-3A model. In inject-mode our `SetAeParam` result is authoritative; the adaptor swap (and `libcus3a` dlopen) is unnecessary. |
| 3 | `maruko_pipeline.c:233` `maruko_disable_userspace3a()` | Replace with `InjectModeEnable(...,{.bInject3A=0})` (or drop) on teardown. |
| 4 | `maruko_cus3a.c` thread | **Unchanged** — keep P1/P4 + `SetAeParam`. Optionally raise `ae_fps` toward full sensor rate now that each tick is cheap → smoother AE with no throttle stepping. AWB stays native (unchanged). |

`CusInject3AEnable_t = { MI_BOOL bInject3A; }` (`mi_isp_hw_dep_datatype.h:106-109`).
Enable AE-only: `MI_ISP_CUS3A_Enable` with `bAE=1,bAWB=0,bAF=0` (AWB native).

### Expected result
- `IspMidThreadWq` 13.2 → ~0, `isp0_P0_MAIN` 16.9 → ~7 → total ≈ **43%** (majestic parity)
  at **full-rate AE** (100 Hz), not throttled.
- Keeps the P1 IIR smoothness + P4 gain-pin already shipped in PR #156.
- Frees `aeFps` from being a CPU knob — it can go to full rate for responsiveness.

## Risks / verification (device .12, bench rules)

1. **Does native AWB still track with no `EnableUserspace3A`?** majestic keeps AWB
   native under inject-mode, so expected yes — verify white balance tracks a
   scene change.
2. **Does the IQ (NR/sharpness/CCM) still reach HW without the agent?** majestic
   proves yes (clean image, no agent). Verify the bin's NR still applies (no
   return of grain) — this is the same `level3DNR`/IQ path, now server-side.
3. **`bAE` vs native-AE contention in inject-mode.** Confirm the injected AE
   result wins and the native AE doesn't fight (watch for oscillation).
4. **Cross-build both backends green** (`make verify` + tests); Star6E untouched.
5. **Cold-start per config** — `aeEngine`/enable-path changes are reinit-forcing
   and respawn-fragile on Maruko (SCL-fence D-state); measure via reboot +
   single cold-start, never live-switch or restart-cycle. See
   `MARUKO_VPE_PORT_INVESTIGATION.md`.
6. **CPU profile** — confirm `IspMidThreadWq`→~0 and total→~43% at 1080p100.

## Rollout shape (proposed)
Add inject-mode as the AE path for `aeEngine=custom` on Maruko (replacing the
`EnableUserspace3A` + no-op-adaptor plumbing), keeping the P1/P4 loop. Keep the
old throttle semantics reachable only if a fallback proves necessary. If
inject-mode lands clean, it becomes the high-fps default (the deferred "wire
custom as default" item is then moot — inject-mode *is* the good default).

## References
- SDK: `mi_isp_cus3a_api.h` (:53 InjectModeEnable, :125-129 agent/EnableUserspace3A),
  `isp_cus3a_if.h` (:503-524 run-mode + framework), `mi_isp_hw_dep_datatype.h`
  (:99-109 enable structs); reference `.../verify/mixer/.../mid_iq_impl.cpp`
  (3020-3070 reg, 3289-3327 SetAeParam loop).
- waybeam current path: `maruko_pipeline.c:201-231` enable, `maruko_cus3a.c:600-633`
  adaptor, `:286-596` thread; Star6E template: `star6e_pipeline.c:307-331`,
  `star6e_cus3a.c:204-458`.
- Measured cause: `MARUKO_VPE_PORT_INVESTIGATION.md` (ablation: throttle collapses
  `IspMidThreadWq` 13→2).

---

## DEVICE VALIDATION (2026-07-04) — CPU thesis PROVEN, but Option B REFUTED

Implemented inject-mode behind an env gate (`MARUKO_AE_INJECT=1`): drop
`EnableUserspace3A` + no-op adaptor, call `MI_ISP_CUS3A_Enable` +
`MI_ISP_CUS3A_InjectModeEnable`, keep the P1 loop driving `SetAeParam`. Cold-start
on .12 @1080p100, `aeFps=100` (full rate):

**CPU — thesis confirmed, spectacularly.** `InjectModeEnable ret=0`.

| | native baseline | **inject + P1 (full-rate)** | majestic |
|---|---|---|---|
| busy | 70.0% | **38.9%** | 42.9% |
| `IspMidThreadWq` | 13.2% | **~0** | ~0 |
| `3A_Proc_0` | 21.9% | **~0** | 24.9% |

−31pt, below majestic, at full-rate AE. The `RegisterIspApiAgent` mechanism is
the whole cost — removing it collapses the ISP apply overhead exactly as predicted.

**Image — Option B ("keep our P1 loop") does NOT work.** The injected-AE image is
**yellow/green cast + very noisy + 85fps** (user-confirmed on the live stream):
- **AWB not applied (yellow/green).** `EnableUserspace3A`'s agent was also pumping
  AWB→HW; `CUS3A_Enable(bAWB=1)` alone in inject-mode does not drive it. Our
  manual path only touches AE.
- **Noise + bad exposure.** Our P1 metering reads `uAvgY≈4` uniformly across the
  32×32 grid on a *lit* scene (device-confirmed: grid is packed at stride nBlkX;
  the divisor bug — `ae_info.AvgBlk*`=0 → /11520 — is fixed, but even the correct
  packed read is ~4, so the stat scale/update is itself wrong). Broken metering
  drives sensor gain to max → noise.
- **85 fps** (not 100) — manual full-rate `SetAeParam` also perturbs pacing.

**Conclusion — the shippable inject path is Option A, not B.** The vendor's native
AE **and** AWB must run in inject-mode (majestic's exact configuration), owning
metering + AWB + the sensor/HW apply, while we only supervise limits (the Star6E
pattern). Manual per-frame `SetAeParam` is a leftover of the agent model and does
not substitute for the vendor 3A apply once the agent is gone. Our P1/P4 controller
(PR #156) remains valuable **only** for the legacy `EnableUserspace3A` throttle
path; it is the wrong tool for inject-mode.

### Next step (revised)
Wire **Option A**: register the vendor/native AE+AWB via the Cus3A framework
(`CUS3A_Init` + `CUS3A_RegInterfaceEX` with the sstar native interface,
adaptor = NATIVE, `CUS3A_SetRunMode(E_CUS3A_MODE_INJECT)`) so the vendor 3A runs
server-side and applies cheaply — no `RegisterIspApiAgent`, no manual `SetAeParam`.
Reference: `.../verify/mixer/.../mid_iq_impl.cpp:3020-3070`; `libcus3a.so` is on the
device. Supervisory thread reduces to Star6E-style limit enforcement only.

The env-gated Option-B prototype (this commit) stays as the CPU proof-of-mechanism;
it is NOT a shippable image. Default behavior is unchanged (gate off).
