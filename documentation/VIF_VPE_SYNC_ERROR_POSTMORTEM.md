# VIF→VPE Sync Error: Investigation Postmortem

## Date: 2026-02-23

## Status: RESOLVED — Fix Verified on Cold Boot

## Symptom

On Star6E (SSC338Q / IMX415), `venc` v0.1.3 (commit bdf40b2) never produces
encoded frames. The encoder loop prints "waiting for encoder data..."
indefinitely. The kernel log floods with:

```
_MI_VIF_EnqueueOutputTaskDev[1340]: layout type 2, bindmode 4 not sync err
```

This error means the VIF output port's buffer layout (type 2) is incompatible
with the binding mode (4 = REALTIME). The VIF rejects every frame enqueue.

## Regression Discovery

Initially believed to be a pre-existing bug since the first commit. User testing
revealed that **v0.1.2 (origin/master) streams correctly** while v0.1.3 does
not.

A/B testing in the same boot cycle confirmed:
- v0.1.2 binary → frames flow immediately
- v0.1.3 binary → hangs at "waiting for encoder data..."

Bisecting v0.1.3 changes: reverting **only** the sensor mode selection
simplification (5-tier → 3-tier) restored streaming in v0.1.3. Both code paths
select the **same sensor mode** — the regression was caused by binary layout
shift triggering undefined behavior from a latent bug.

## Root Cause: VPE Struct Size Mismatch

The reference HAL (`sdk/ssc338q/hal/star/i6_hal.c`) uses different VPE structs
for infinity6e (Star6E) vs base infinity6:

- **infinity6 (base):** `i6_vpe_chn` (~104 bytes), `i6_vpe_para` (~28 bytes)
- **infinity6e (Star6E):** `i6e_vpe_chn` (~192 bytes), `i6e_vpe_para` (~120 bytes)

The `i6e` variants include additional fields:
- `i6e_vpe_chn` adds `i6e_vpe_ildc lensInit` between `iqparam` and `lensAdjOn`
- `i6e_vpe_para` adds `i6e_vpe_ldc lensAdj` between `reserved[16]` and `hdr`,
  which **shifts all subsequent field offsets**

Our `star6e.h` typedefs mapped to the smaller base structs:
```c
typedef i6_vpe_chn   MI_VPE_ChannelAttr_t;  // WRONG for infinity6e
typedef i6_vpe_para  MI_VPE_ChannelParam_t;  // WRONG for infinity6e
```

When the smaller struct is passed to `MI_VPE_CreateChannel()`, the driver (which
expects `i6e_vpe_chn` on infinity6e) reads past the struct boundary into stack
garbage. For `i6e_vpe_para`, the inserted `lensAdj` field shifts ALL offsets —
the driver reads `hdr` where it expects `lensAdj`, `level3DNR` where it expects
`hdr`, etc.

### Why v0.1.2 Worked Despite the Bug

The struct mismatch is **undefined behavior** — the driver reads uninitialized
stack memory past the struct boundary. In v0.1.2's binary layout, the stack
garbage happened to contain values the driver could tolerate. The v0.1.3 sensor
mode selection change shifted the stack layout just enough to change what garbage
was read, breaking VPE initialization. This is classic UB: "works by accident."

### Why Majestic Works

Majestic loads the HAL dynamically (`dlopen`). The HAL detects `INFINITY6E` at
runtime and uses the correct `i6e_vpe_chn` / `i6e_vpe_para` structs. We linked
against the SDK directly with hardcoded typedef aliases that pointed to the wrong
(smaller) struct definitions.

## What Was Tried (and Failed Before Root Cause Found)

### Attempt 1: Remove I6_SYS_LINK_LOWLATENCY from VIF→VPE binding
- **Result:** FAILED. `layout type 2` persists — it's set by VPE init, not
  binding flags.

### Attempt 2: Remove MI_SYS_SetChnOutputPortDepth on VIF/VPE ports
- **Result:** FAILED. Same sync error.

## Fix Applied

### 1. VPE struct typedefs (root cause fix)

`star6e-standalone/include/star6e.h`:
```c
/* infinity6e (Star6E) driver expects the larger i6e_ struct variants which
   include additional lens-correction fields.  Using the smaller i6_ structs
   causes the driver to read past the struct boundary into stack garbage,
   corrupting VPE init and breaking the VIF→VPE REALTIME link. */
#if defined(PLATFORM_STAR6E)
typedef i6e_vpe_chn  MI_VPE_ChannelAttr_t;
typedef i6e_vpe_para MI_VPE_ChannelParam_t;
#else
typedef i6_vpe_chn   MI_VPE_ChannelAttr_t;
typedef i6_vpe_para  MI_VPE_ChannelParam_t;
#endif
```

### 2. Binding flag cleanup (secondary, matches HAL)

`star6e-standalone/src/backend_star6e.c` — VIF→VPE binding:
```c
// Before:
MI_SYS_BindChnPort2(&vif_port, &vpe_port, ..., I6_SYS_LINK_REALTIME | I6_SYS_LINK_LOWLATENCY, 0);
// After:
MI_SYS_BindChnPort2(&vif_port, &vpe_port, ..., I6_SYS_LINK_REALTIME, 0);
```

### 3. Remove spurious buffer depth calls (secondary, matches HAL)

Removed `MI_SYS_SetChnOutputPortDepth` calls on VIF and VPE output ports.
The reference HAL does not call this function; the VENC port depth call remains.

## Verification

Cold-boot test on Star6E (no majestic residue):
```
venc -s 1920x1080 -f 60 -c 265cbr -h 192.168.2.2 -p 5600 --ae-off --awb-off --vpe-3dnr 0 --verbose
```
- Frames flowing within seconds of start
- No VIF sync errors in dmesg
- `layout type 2, bindmode 4 not sync err` message completely gone

## Key Learnings

1. **Always cold-boot test.** Majestic residue masks pipeline bugs.
2. **HAL struct sizes matter.** The HAL has platform-specific struct variants
   (`i6_*` vs `i6e_*`); typedefs must match the target SoC's expectations.
3. **The reference HAL is the source of truth** for struct types, field orders,
   function call sequences, and binding flags.
4. **UB from struct mismatches can appear stable for months** — any binary layout
   change (new variables, different optimization) can trigger the latent bug.
5. **Bisect by reverting individual changes** in A/B tests within the same boot
   cycle to isolate binary-layout-dependent failures.
6. **The `i6e_vpe_para` mid-struct insertion is particularly insidious** — it
   doesn't just add fields at the end, it shifts all subsequent field offsets,
   silently corrupting driver reads.
