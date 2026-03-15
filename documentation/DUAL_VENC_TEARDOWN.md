# Dual VENC Teardown Design

## Problem

When the venc process receives SIGTERM while dual VENC (gemini mode) is
active, the SigmaStar VPE kernel driver can enter uninterruptible disk
sleep (D-state).  Once in D-state, the process is unkillable — even
`_exit()` blocks because the kernel can't release the driver's internal
locks during process cleanup.

## Root Cause

The VPE port 0 fans out to two VENC channels (ch0 for streaming, ch1 for
recording).  `MI_VENC_StopRecvPic` triggers a VPE flush that waits for all
consumers to return their buffers.  Two race conditions cause the hang:

1. **Backpressure race:** After the main loop exits (`g_running=0`), ch0
   frames stop being consumed.  At 120fps the 12-frame buffer fills in
   ~100ms.  If `StopRecvPic` runs after the buffer fills, the VPE flush
   enters D-state waiting for buffers that will never be drained.

2. **Lock contention:** `MI_SYS_UnBindChnPort` and `MI_VENC_GetStream`
   (called by the recording thread) contend for the same kernel-side VPE
   lock.  Running them concurrently causes intermittent deadlock.

## Solution

### Teardown Sequence

```
SIGTERM received:
  g_running = 0           ← main loop exits
  fork watchdog child     ← safety net (see below)
  cus3a_request_stop()    ← signal 3A thread (non-blocking)

pipeline_stop():
  teardown peripherals    ← IMU, audio, output (no ISP interaction)
  rec_thread stop + join  ← recording thread exits (no concurrent GetStream)
  unbind VPE→VENC ch1     ← safe: no concurrent consumers
  unbind VPE→VENC ch0     ← safe: main loop already exited
  drain ch0 + ch1         ← flush remaining buffered frames
  StopRecvPic ch1         ← VPE is unbound, no flush wait
  StopRecvPic ch0
  unbind VIF→VPE
  destroy channels, VPE, VIF, sensor
```

Key ordering rules:
1. **Recording thread must stop before unbind** — prevents GetStream/UnBind
   lock contention deadlock.
2. **Unbind before StopRecvPic** — prevents VPE flush backpressure.
3. **Pipeline stop before recorder file close** — the recording thread
   needs the ts_recorder fd open until it's joined.

### Recording Thread Shutdown

The recording thread (`dual_rec_thread_fn`) checks `d->rec_running` at the
top of each loop.  When `g_running==0`, it uses non-blocking `GetStream`
(timeout=0) so it exits within one iteration (~1ms).  Skips SD card writes
during shutdown to avoid blocking on flash GC.

### Watchdog Child Process

A forked child process acts as a safety net:

```
fork()
child:
  for 8 seconds: poll parent liveness every 1s
    if parent gone: _exit(0)              ← normal exit, no orphan
  if parent still alive:
    SIGKILL parent                        ← force kill if teardown hung
    wait 3s
    if parent STILL alive:
      write "b" to /proc/sysrq-trigger   ← emergency reboot
```

The watchdog has zero overhead during normal operation — it only runs during
the shutdown path.  If the parent exits normally (1-2s), the child detects
`kill(parent, 0) != 0` on the next 1-second poll and exits immediately.

### CUS3A Thread Split Stop

The custom 3A thread (AE+AWB) can block in ISP kernel calls
(`GetAeHwAvgStats`, `GetAeStatus`).  Joining it before pipeline drain would
delay the drain and cause backpressure.  The stop is split:

- `star6e_cus3a_request_stop()` — sets `running=0` (non-blocking)
- Pipeline drain + stop runs while the thread is exiting
- `star6e_cus3a_join()` — waits for thread exit after pipeline is stopped

## Verification

Tested on Star6E imx335:

| Test | Rounds | Result |
|------|--------|--------|
| Dual 30fps stream + 120fps record, SIGTERM | 10/10 | Clean exit 1s |
| Mixed modes (dual/mirror/stream), all signals | 15/15 | Clean exit 1-3s |
| Varied FPS (30/60/90/120), dual + record | 15/15 | Clean exit 1-3s |
| Rapid start/stop (10s runs, no settle) | 15/15 | Clean exit 1s |

## Known Limitations

- The SigmaStar VPE kernel driver has no timeout on internal flush waits.
  If the teardown sequence is incorrect, the driver enters D-state with no
  userspace recovery.  The watchdog's sysrq-b is the last resort.
- `MI_SNR_Disable` during active VPE can itself D-state — the sensor must
  be stopped after VPE is unbound, not before.
