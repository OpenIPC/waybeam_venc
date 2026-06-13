# Star6E single-PID (in-process) reinit — findings

**Status: closed, negative result (2026-06-07).** Same-PID pipeline reinit is
not achievable on the SigmaStar I6E in userspace. The fork+exec respawn
(`src/venc_respawn.c`) is a **driver requirement**, not a design preference.

This document records *why*, so the dead ends are not re-attempted without new
SigmaStar SDK/kernel insight. It supersedes the earlier "the storm is intrinsic
to VENC destroy+rebind, process-model-independent" framing in `CRASH_LOG.md`.

## Background

`MUT_RESTART` config changes (resolution, sensor mode, ref-pred, framing, etc.)
require rebuilding the encoder pipeline. On Star6E the only reliable mechanism is
fork+exec: the parent tears down + `MI_SYS_Exit`s + exits, a fresh child
`MI_SYS_Init`s and rebuilds. The goal of this investigation was to rebuild in the
same PID (faster, no inherited-fd leak), gated behind `VENC_INPROC_REINIT` so the
default behaviour was never touched.

## Result

Every in-process reset lever was implemented and bench-tested on imx335 @
192.168.1.13, with reboot-surviving capture (`dmesg -c` drained to the SD card
with `sync`, daemon stdout to `/mnt/mmcblk0p1/`, since the deadline watchdog's
`sysrq-b` wipes tmpfs and the dmesg ring):

| Reset strategy | Result |
|---|---|
| `MI_ISP_DisableUserspace3A(0)` before VPE destroy | Wedge eliminated; fd/RSS flat over 14 reinits — **but stream dead from cycle 2** (ISP readiness timeout, VIF not-sync, 0 RTP egress). |
| `MI_SYS_Exit` + `MI_SYS_Init` in same PID | The historical "2nd `MI_SYS_Init` hangs `MI_DEVICE_Open`" mode is **gone** — it re-inits cleanly now. But ISP/VIF still fail to come up. |
| close `/dev/mi_vif` + `/dev/mi_vpe` fds in-process | **Does not deadlock** (the PR #120 deadlock is `/dev/mi_sys`-specific). Insufficient — rebuild still wedges (`EnsureInputPortFifoEmpty no response 1000ms`). |
| `dlclose` + `dlopen` all MI vendor libs (reset lib globals) | Completes; `MI_SYS` re-inits — but `star6e_pipeline_start` still hangs (`GetInputPortInfo not found`, `VPE chn not create`). |

## Mechanism

Resetting MI_SYS, the VIF/VPE fds, **and** the MI vendor lib process-globals in
one process is still insufficient. The residual state the rebuild needs is the
VIF/VPE/ISP **channel state in the kernel driver**, pinned to the task and
released only by `execv` (fresh address space + fd context within the same PID
slot). Userspace cannot reach it. This mirrors the earlier MMU-storm conclusion:
the whole VENC/VIF/VPE/ISP rebuild class requires a process boundary on Star6E.

The in-process failure signature, for recognition: `ISP channel readiness timeout
after 2000 ms` → `MI_ISP_*CmdLoadBinFile failed -1` → `_MI_VIF_EnqueueOutputTaskDev:
layout type 2, bindmode 4 not sync err` (floods) → no frames.

## Corrections to earlier notes

- The MMU read-fault storm (`ClientId=0x15 IsWrite=0`) is **fixed** for both the
  respawn and in-process paths (the `debug_osd_destroy` settle, PR #126). It did
  not fire in any of the above runs.
- A second `MI_SYS_Init` in the same PID **no longer hangs** `MI_DEVICE_Open`.
- The `/dev/mi_*` close deadlock (PR #120) is `/dev/mi_sys`-**specific**;
  `/dev/mi_vif` and `/dev/mi_vpe` close cleanly in-process.

## Decision

Keep fork+exec respawn. Do not re-attempt in-process reinit without new SigmaStar
SDK/kernel insight into releasing the per-task VIF/VPE/ISP channel state.
