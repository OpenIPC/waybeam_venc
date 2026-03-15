# AE Porting Minimal Delta From 99301fe

This file describes the minimum functional changes needed to take the repo at
upstream commit `99301fe` and reproduce the current working AE behavior:

- working AE on normal modes (`30`, `60`, `120 fps`)
- working AE on the special IMX335 `2400x1350 @ 90 fps` path

This is written for an agent porting the behavior into a codebase that is
still close to that upstream snapshot.

## Baseline

Use this exact upstream base:

- repo commit: `99301fe`

Do not use the current full working tree as the port target. This file is
specifically about the smallest AE-related delta from `99301fe`.

## High-Level Split

The minimum port is not a single global AE change. It is two paths:

1. Normal modes (`30/60/120 fps`)
   - keep SigmaStar internal AE as the owner
   - avoid over-driving CUS3A after ISP bin load/startup

2. Special `90 fps` mixed path on IMX335 `2400x1350`
   - use `userspace` AE mode
   - defer userspace AE startup until encoded frames exist
   - actively keep userspace 3A quiesced before that point so vendor AE does
     not wake early at the wrong cadence

If the port does not preserve that split, it will likely regress either:

- normal-mode AE liveness
- or 90 fps startup cadence

## Files That Matter

Minimum source scope:

- [backend_star6e.c](/mnt/c/joakim_vencmar10/star6e-standalone/src/backend_star6e.c)
- [venc_config.h](/mnt/c/joakim_vencmar10/star6e-standalone/include/venc_config.h)
- [venc_config.c](/mnt/c/joakim_vencmar10/star6e-standalone/src/venc_config.c)
- [venc_api.c](/mnt/c/joakim_vencmar10/star6e-standalone/src/venc_api.c)
- [venc_api.h](/mnt/c/joakim_vencmar10/star6e-standalone/include/venc_api.h)
- config JSON defaults only if the target repo still lacks `isp.aeMode`

Most of the real behavior lives in `backend_star6e.c`.

## Required Functional Changes

### 1. Add Explicit AE Ownership Mode

Add `isp.aeMode` support with at least:

- `internal`
- `userspace`

Default should remain:

- `internal`

Why:

- normal modes work best when they preserve the original internal-AE behavior
- the 90 fps path needs a different startup model

This is a small config/plumbing change. It is not the hard part.

### 2. Restore the Normal-Mode Internal-AE Baseline

For non-high-FPS modes, do not force CUS3A ownership after startup.

The important behavioral rule is:

- normal internal-AE path should load ISP state and then leave ISP AE in its
  own internal auto mode

What to avoid on the normal path:

- unconditional post-bin `CUS3A_Enable(1,1,1)`
- unconditional startup `enable_userspace_3a`
- periodic CUS3A forcing in normal internal mode

Expected result:

- AE on `30/60/120 fps` responds again
- this was the key reason `60 fps` recovered in the current repo

### 3. Keep the High-FPS Detection Narrow

Keep the special path scoped to the real mixed profile only.

Current effective rule:

- actual sensor fps `>= 90`
- sensor mode output smaller than capture plane / overscan path active

This is intended to catch the IMX335 `2400x1350 @ 90 fps` mode without
polluting ordinary modes.

### 4. For the 90 fps Path, Do Not Start Userspace AE During Pipeline Init

On the high-FPS `userspace` path:

- do not let userspace AE become active during ordinary startup
- do not start it during ISP bin load/startup sequencing
- mark it as deferred until the first encoded frame exists

This requires state in the pipeline object roughly equivalent to:

- deferred userspace start pending
- userspace start completed
- exposure-cap reapply countdown

### 5. Quiesce Userspace 3A Before the First Encoded Frame

This is the critical part for the 90 fps path.

Before encoded output is available, keep userspace 3A explicitly disabled in
the idle pre-frame loop.

The important detail is cadence:

- a coarse interval around `66 ms` is not enough
- the working result came from a much tighter interval around `5 ms`

Why this matters:

- with coarse pacing, vendor userspace AE wakes early and logs
  `AeInit ... = 54` or `71`
- with tight pacing, the same path initializes at
  `AeInit ... = 90`
  `DoAe ... = 90`

In practice, the port needs:

- a helper that calls `MI_ISP_DisableUserspace3A()`
- that helper invoked repeatedly while:
  - deferred userspace start is pending
  - and the encoder has not yet produced a frame

### 6. Start Userspace AE Only After the First Encoded Frame

Once encoded output exists:

- call `MI_ISP_EnableUserspace3A()`
- mark userspace AE as started
- stop the pre-start quiesce loop

This must happen after the first real frame is available, not merely after the
pipeline has been bound.

### 7. Reapply the Exposure Cap After Userspace Start

After deferred userspace AE starts on the 90 fps path:

- reapply the configured `isp.exposure` cap immediately
- then reapply it again a few times over the next few frames

Practical effect in the current repo:

- `10 ms` cap stays live
- `exposure_limit.max_shutter_us = 10000`

Without this, vendor userspace AE can restore its own larger shutter ceiling.

### 8. Keep the Existing Sensor/Bind/Unlock 90 fps Tricks

Do not remove the existing high-FPS bring-up path that already made 90 fps
streaming work.

That includes:

- IMX335 unlock sequence
- overscan handling
- relaxed bind behavior used on the mixed profile

The AE fix is layered on top of those, not a replacement for them.

## What Is Not Required For The Minimal Port

These were useful during investigation but are not required for the smallest
functional port:

- full custom AE callback implementation
- `CUS3A_RegInterface(EX)` experiments
- permanent periodic userspace CUS3A keepalive after AE has started
- investigation-only diagnostics beyond what the target repo already has

The minimum working result came from controlling startup timing and preserving
the exposure cap, not from implementing a custom AE algorithm.

## Expected Runtime Results

### Normal Modes

On `30/60/120 fps`, the normal internal-AE path should:

- come up without forced userspace AE ownership
- show live AE movement when the scene changes

### Special 90 fps Mode

On IMX335 `2400x1350 @ 90 fps`, the working result should show:

```text
AE    : userspace
AE init: deferred userspace startup after first encoder frame
[    AeInit] FPS ... = (   90,   90,   90,   90,   90)
[      DoAe] FPS ... = (   90,   90,90000,   90)
```

If it still shows `54` or `71`, the pre-start userspace quiesce behavior is
not yet correct.

## Verification Checklist

1. Normal internal-AE path:
   - run `30 fps`
   - run `60 fps`
   - run `120 fps`
   - confirm `/api/v1/ae` or equivalent telemetry changes under scene changes

2. Special `90 fps` path:
   - run IMX335 mode `2`
   - confirm startup logs `AeInit = 90` and `DoAe = 90`
   - confirm exposure cap remains `10000 us`
   - confirm AE values change over time under scene changes

3. Failure signatures:
   - `AeInit = 54` or `71`:
     pre-start quiesce is too weak or userspace AE is still waking too early
   - `max_shutter_us` jumps above `10000`:
     cap reapply after userspace start is missing or insufficient
   - normal `60 fps` AE is static:
     normal internal path is still over-driving CUS3A

## Smallest Practical Port Order

For another agent, the least risky order is:

1. Add `isp.aeMode` plumbing.
2. Restore normal-mode internal-AE behavior first and confirm `60 fps` works.
3. Add deferred userspace startup for the 90 fps path.
4. Add tight pre-start userspace quiesce at about `5 ms`.
5. Add post-start exposure-cap reapply.
6. Re-test all target modes.

This order is important because it isolates the plain internal-AE recovery
from the special 90 fps userspace startup behavior.
