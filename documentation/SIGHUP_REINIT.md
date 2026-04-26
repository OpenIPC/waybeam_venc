# SIGHUP Reinit and Live Reload (Star6E)

## Overview

`venc` reloads `/etc/venc.json` and rebuilds its full pipeline without a
process restart.  Sensor mode, codec, resolution, FPS, output settings —
all of these are honored by reload.

| Trigger | Behaviour |
|---------|-----------|
| `killall -1 venc` (SIGHUP) | Reload disk config, full teardown + start |
| `GET /api/v1/restart` | Same as SIGHUP (no save first) |
| `GET /api/v1/defaults` | Save defaults to disk, full teardown + start |
| `GET /api/v1/set?<MUT_RESTART>` | Save field to disk, full teardown + start |
| `GET /api/v1/set?<MUT_LIVE>` | In-place via `star6e_controls.c`, no reinit |

There is one reinit path.  No diff detection, no priority queue between
SIGHUP-from-disk vs API-in-memory — every request reloads the on-disk
config and rebuilds from sensor up.

## Cycle

```
star6e_runtime_handle_reinit()
  └─ star6e_runtime_restart_pipeline()
       1. star6e_cus3a_request_stop / star6e_controls_reset / star6e_cus3a_join
       2. star6e_pipeline_stop()                 ← full teardown to sensor
            └─ star6e_pipeline_reset_persist()   ← g_isp_initialized,
                                                   g_last_isp_bin_path,
                                                   g_cus3a_handoff_done
       3. venc_config_defaults() + venc_config_load(/etc/venc.json)
       4. star6e_pipeline_start()                ← cold start
       5. star6e_runtime_apply_startup_controls()
       6. install_signal_handlers()
```

Any non-zero return propagates to `star6e_runner_run()`, which exits the
process with that code.  The watchdog fork in `star6e_runner_teardown()`
escalates to `kill -9` and `sysrq-b` if teardown itself hangs.

## Hard fail, no fallback

Per the operator design:

- Single path: full teardown + reload + start.  No partial reinit.  No
  state machine.
- Hard fail: any teardown or start failure exits with non-zero.  No
  silent partial fallback.

## Bench-validated working envelope

| Pattern | Status |
|---|---|
| Single mode change (e.g. 30 fps → 90 fps) | works |
| Up to ~4 consecutive cross-mode SIGHUPs | works (1.5–4.5 s per cycle) |
| 5+ consecutive cross-mode SIGHUPs | **degrades** — VIF reports `layout type 2 bindmode 4 not sync err`, encoder stops producing frames, next SIGHUP zombies |

The degradation appears to be the SigmaStar VIF/MIPI subsystem failing
to fully reset after repeated `MI_SNR_Disable`/`Enable` cycles within
a short window.  Once degraded, recovery requires `echo b >
/proc/sysrq-trigger` (see `CRASH_LOG.md`).

In practice, an operator changes sensor mode rarely — this release
covers that workflow.  Torture-style rapid rotation is not supported
on this BSP.

## Timeouts

| Where | Value |
|---|---|
| `handle_signal()` shutdown `alarm()` | 2 s |
| `star6e_runner_teardown()` watchdog poll | 6 × 500 ms |
| `star6e_runner_teardown()` post-`SIGKILL` grace | 1 s |
| `star6e_pipeline_wait_isp_channel()` cap | 2000 ms (kept) |
| `drain_venc_channel()` per channel | 150 ms |
| ISP `dlopen` fallback `usleep` | 100 ms (kept) |

Total successful cycle: 1.5–3.5 s on Star6E in bench testing (mode-
dependent — higher FPS modes take longer because more sensor/ISP
warmup is needed).  Maruko cold start is ~1 s; Star6E rebuilds more
state.

## Historical: why partial reinit existed (0.3.2 – 0.8.x)

HISTORY 0.3.2 backed out a full teardown path because `MI_SNR_Disable` +
`MI_SNR_Enable` cycles on the I6E MIPI D-PHY put VIF into a fault state,
hung `MI_VENC_StopRecvPic`, and put the process in D-state.  The stopgap
was `star6e_pipeline_stop_venc_level()` — tear down VENC/output/audio
but keep sensor/VIF/VPE running across reinit.

That stopgap survived for several releases because `MI_SNR_Disable` was
considered unrecoverable.  Several subsequent fixes — drain-before-
`StopRecvPic` ordering, the watchdog fork, `pre_init_teardown`, the
VPE SCL clock preset — collectively made the full sensor cycle
survivable, and the partial path was retired.

If a regression reintroduces the D-state hang on `MI_SNR_Disable`,
restoring the partial path is a documented escape hatch (see git
history before 0.9.0 for `star6e_pipeline_stop_venc_level()` and
`star6e_pipeline_reinit()`).

## ISP channel readiness

`MI_VPE_CreateChannel` starts ISP channel initialisation asynchronously.
Any ISP API call issued before the kernel logs `MhalCameraOpen` will
receive `[MS_CAM_IspApiGet][ERROR - ISP channel [0] have NOT been
created.`  Two guards prevent this:

1. `star6e_pipeline_wait_isp_channel()` — polls
   `MI_ISP_IQ_GetParaInitStatus` for up to 500 ms after a new VIF→VPE
   bind is established.  Fires every reinit (every reinit destroys VPE).
2. `star6e_pipeline_wait_isp_ready()` — polls again before the ISP bin
   load writes registers.

## GCC `flatten` attribute

`star6e_pipeline_start()` is annotated with `__attribute__((flatten))`.
The SigmaStar I6E ISP driver inspects the call-stack layout at the
moment `MI_VPE_CreateChannel` is called.  At `-Os`, GCC may emit
`start_vpe()` as a separate out-of-line function, changing the stack
layout and causing `MI_ISP_IQ_GetParaInitStatus` to return error 6.
`flatten` forces all static callees inline, restoring the monolithic
stack frame the driver expects.
