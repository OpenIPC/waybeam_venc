# Crash Log — venc on Star6E

Per-incident notes on hangs, D-state, and recovery actions on the bench.

## 2026-04-26 — CamOsMutexLock D-state during full-teardown reinit (v0.9.0 dev)

**Bench:** `root@192.168.1.13` — SSC338Q + IMX335.

**Trigger:** Cross-mode SIGHUP rotation under v0.9.0 full-teardown reinit
without the audio AI persist hack.  After mode 1 → mode 2 (60 → 90 fps)
SIGHUP, the venc process entered D-state.  `/proc/<pid>/wchan` reported
`CamOsMutexLock`; load average climbed past 13; subsequent SIGINT/SIGTERM
to venc printed `> Force exiting.` but the process did not actually exit
because `_exit()` cannot complete from D-state.

**Root cause:** the v0.9.0 plan assumed `star6e_pipeline_stop()` is a
true cold teardown.  It is not — `MI_SYS_Init` / `MI_SYS_Exit` only fire
in `star6e_runner_init` / `star6e_runner_teardown`, so the kernel AI/ISP
driver state survives reinit.  Cycling `MI_AI_Disable` / `MI_AI_Enable`
on a kernel-tracked AI device deadlocks `CamOsMutexLock` after a few
iterations.  The `g_ai_persist` hack in `star6e_audio.c` exists
specifically to skip that cycle.  The same logic applies to
`g_isp_initialized` (CUS3A enable deadlock) and `g_last_isp_bin_path`
(reloading the bin pins IMX335 at ~100 fps).

**Fix:** restore the audio persist guard and replace the in-process reinit
with process-level fork+exec respawn (see `documentation/SIGHUP_REINIT.md`).
`star6e_audio_teardown()` keeps `g_ai_persist.initialized` set so the kernel
AI device is never user-space disabled — kernel cleanup on process exit
handles it.  `star6e_pipeline_stop()` clears all three userspace flags
(`g_isp_initialized`, `g_last_isp_bin_path`, `g_cus3a_handoff_done`) since
the next `pipeline_start` always runs in a fresh PID with cold kernel state.

**Recovery:** `echo b > /proc/sysrq-trigger` over SSH unhung the device.
Surprisingly, this can succeed even when venc is in D-state and the
shell appears responsive on a hot SSH session — the write to
`sysrq-trigger` runs in the SSH command's context (not venc's), and
sysrq is a kernel-level emergency reboot that bypasses normal task
state.  **If a normal `reboot` hangs and pidof shows venc still alive
in D-state, try `echo b > /proc/sysrq-trigger` from a fresh SSH session
before requesting a power cycle.**  Caveat: filesystem dirty pages are
lost — only use after `sync` if persistent state matters.
