# SIGHUP Reinit and Live Reload (Star6E)

## Overview

`venc` reloads `/etc/venc.json` and rebuilds its full pipeline without
operator intervention.  Sensor mode, codec, resolution, FPS, output
settings — all are honored by reload.

| Trigger | Behaviour |
|---------|-----------|
| `killall -1 venc` (SIGHUP) | Cold restart via fork+exec respawn |
| `GET /api/v1/restart` | Same as SIGHUP |
| `GET /api/v1/defaults` | Save defaults to disk, then respawn |
| `GET /api/v1/set?<MUT_RESTART>` | Save field to disk, then respawn |
| `GET /api/v1/set?<MUT_LIVE>` | In-place via `star6e_controls.c`, no respawn |

There is one reinit path: process-level cold restart.  No partial reinit,
no in-process state machine, no priority queueing between SIGHUP and API.

## Why a process-level restart, not in-process

The Phase 1 plan called for in-process pipeline rebuild
(`MI_SYS_Exit` + `MI_SYS_Init`).  Bench testing on `192.168.1.13`
(SSC338Q + IMX335) disproved that approach:

- **`MI_SYS_Exit` + `MI_SYS_Init` in same PID is broken.**  The
  SigmaStar driver retains "already_inited" flags tied to the process
  PID.  A second `MI_SYS_Init` trips `MI_DEVICE_Open` hangs and the
  VPE warns `no wakeup event for more than 5 seconds!`.
- **Partial teardown without `MI_SYS_Exit` survives ~4 cycles.**  After
  4–5 sensor mode SIGHUPs the VIF driver starts rejecting binds with
  `_MI_VIF_EnqueueOutputTaskDev[1340]: layout type 2, bindmode 4 not
  sync err`, the encoder stops getting frames, and the next SIGHUP
  zombies into D-state on `CamOsMutexLock`.
- **Process restart works for any number of cycles.**  Empirical test:
  `killall venc; sleep 0.5; venc &` repeated 12 times across 4 sensor
  modes — 100 % pass.  6 rounds (24 cycles): 100 % pass.

The kernel driver releases per-PID state correctly when a process dies.
A fresh PID sees a clean SDK.  The cleanest way for venc to ride that
path on SIGHUP is to fork+exec a successor.

## Cycle

```
star6e_runtime_handle_reinit()
  1. set g_respawn_after_exit = 1
  2. set g_running = 0  ── exits the main run loop

star6e_runner_run() returns ──► backend_execute returns to main()
  3. backend_ops->teardown(ctx)  ── normal clean shutdown:
       cus3a stop, pipeline_stop, recorder/audio/IMU teardown,
       MI_SYS_Exit, VPE SCL preset, mi_deinit, watchdog fork
  4. main() checks star6e_runtime_respawn_pending()  ── true
  5. star6e_runtime_respawn_after_exit():
       fork() → child sleeps 500 ms (lets kernel reap parent),
       resets signal mask, reopens /tmp/venc.log,
       execv("/proc/self/exe", {"venc", NULL})
  6. parent returns from main() → process exits cleanly

  Child becomes the new venc with a fresh PID, runs the normal cold-
  start sequence (mi_init, MI_SYS_Init, sensor unlock, ISP bin load,
  pipeline_start, apply_startup_controls).
```

The fork happens **after** the parent's full teardown, not before.  If
the child were created earlier it would inherit the parent's MI device
fds and the kernel's per-PID cleanup wouldn't fully fire when the
parent calls `MI_SYS_Exit`.

The watchdog forked inside `star6e_runner_teardown` renames itself via
`prctl(PR_SET_NAME, "venc-wd")` so the new venc's
`is_another_venc_running()` skips it.  Without this rename the
respawned process sees a still-alive `comm == "venc"` watchdog and
exits with "venc already running".

## Bench-validated working envelope

Verified on Star6E SSC338Q + IMX335 @ `192.168.1.13`:

| Pattern | Result |
|---|---|
| Single mode change SIGHUP | works |
| Cross-mode rotation 0→1→2→3, 3 rounds (12 cycles) | 100 % pass |
| Cross-mode rotation 0→1→2→3, 6 rounds (24 cycles) | 100 % pass |
| Cycle time (SIGHUP → "Respawning" log line) | 393–795 ms |
| Cycle time (SIGHUP → new venc HTTP up) | ~13 s (cold init) |
| `dmesg` faults | 0 |

Cold init dominates the perceived restart latency.  The HTTP API is
unreachable for ~13 seconds during respawn — operators changing sensor
mode rarely (the typical case) accept this; the alternative
(in-process partial reinit) is fast for the first 4 cycles and broken
forever after.

## Persist hacks (still required)

`g_ai_persist` in `star6e_audio.c` keeps the kernel AI device alive
across the audio teardown call inside `pipeline_stop`.  Cycling
`MI_AI_Disable` on a kernel-tracked device deadlocks `CamOsMutexLock`,
which would hang `pipeline_stop` past the watchdog window and trigger
a sysrq-b reboot.  The kernel cleans up AI state on process exit
regardless of whether userspace called `MI_AI_Disable`, so the persist
guard is safe.

`g_isp_initialized` and `g_last_isp_bin_path` in `star6e_pipeline.c`
are cleared in `pipeline_stop`.  No load-bearing reason to skip — the
ISP/CUS3A subsystem is always paired with `MI_SYS_Exit` in the same
shutdown.

## ISP channel readiness

`MI_VPE_CreateChannel` starts ISP channel initialisation
asynchronously.  Any ISP API call before the kernel logs
`MhalCameraOpen` returns `[MS_CAM_IspApiGet][ERROR - ISP channel [0]
have NOT been created.`  Two guards prevent this:

1. `star6e_pipeline_wait_isp_channel()` polls
   `MI_ISP_IQ_GetParaInitStatus` for up to 2000 ms after a new VIF→VPE
   bind is established.
2. `star6e_pipeline_wait_isp_ready()` polls again before the ISP bin
   load writes registers.

Both timeouts kept at the original SDK-tested values — bench testing
showed cuts to 500 ms cause "ISP channel readiness timeout" warnings
on some imx335 modes.

## GCC `flatten` attribute

`star6e_pipeline_start()` is annotated with `__attribute__((flatten))`.
The SigmaStar I6E ISP driver inspects the call-stack layout at the
moment `MI_VPE_CreateChannel` is called.  At `-Os`, GCC may emit
`start_vpe()` as a separate out-of-line function, changing the stack
layout and causing `MI_ISP_IQ_GetParaInitStatus` to return error 6.
`flatten` forces all static callees inline, restoring the monolithic
stack frame the driver expects.

## Recovery

If a respawn flow somehow gets wedged (e.g. the parent's watchdog
escalates to sysrq-b because pipeline_stop hung in a kernel D-state),
the device reboots cleanly and S95venc starts a fresh venc.  See
`documentation/CRASH_LOG.md` for the `echo b > /proc/sysrq-trigger`
remote-recovery trick used during bring-up of this design.
