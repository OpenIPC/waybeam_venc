# Crash Log — venc on Star6E

Per-incident notes on hangs, D-state, and recovery actions on the bench.

## 2026-05-21 — Teardown fd-cleanup cherry-pick from PR #122/#123

**Scope:** the SoC/MI-independent teardown fixes extracted from the DIS
stabilization work (PR #122) and the MMU-storm investigation (PR #123),
without the stab feature, the in-process reinit experiment (disproven on
device, see below), or the reboot-gate.

**Fix 1 — VENC teardown order (`star6e_pipeline_stop`).**  Star6E unbound
VPE→VENC *before* stopping VENC, so for the teardown window the kernel SDK
kept encoding/flushing a buffered frame out of a port userspace had already
ripped out.  VENC (MMU client 0x15) then reads a freed VPE buffer —
`_MI_SYS_MMU_Callback Status=0x2 IsWrite=0` — which storms into a hardware
watchdog reset on the ~2nd rapid respawn.  Reproduced on master from a cold
boot, so it predates the stab/framing work.  Reordered to **StopRecvPic →
drain output → unbind VPE→VENC → unbind VIF→VPE → destroy → stop
VPE/VIF/sensor**, matching the already-fixed Maruko path
(`maruko_pipeline_teardown_graph`, fixed S1 bench 2026-05-15 for the same
root cause as a page fault in `MI_SYS_IMPL_FlushInputPortTasks`).  Star6E had
never been given that fix.

**Fix 2 — cold-init VIF/VPE on sensor-mode change.**  On a `video0.size`
change the respawn child inherits `/dev/mi_vif` + `/dev/mi_vpe` fds that pin
the OLD sensor mode's kernel state; the fresh process then re-inits VIF to a
different mode against that stale state and wedges `[vpe0_P0_MAIN]`.  The
runtime now snapshots the started base size and, on a size change only,
latches `venc_respawn_set_cold_vif(1)` so the fd-scrub closes just those two
fds (never `/dev/mi_sys` — closing it mid-teardown is the confirmed
deadlock).  Same-size respawns keep the deadlock-safe inherited-fd default.

**Known limitation NOT fixed here (SoC/MI):**  same-mode respawns can still
hit the MMU read-fault storm (client 0x15, `IsWrite=0`) on the ~2nd
consecutive respawn.  PR #123 proved this is **intrinsic to destroying +
rebinding a VENC channel against the live VPE port on this SoC, independent
of process model** — an in-process rebuild storms too.  Neither the teardown
reorder nor the cold-vif scrub removes it; they remove the teardown-time
D-state wedge and the cross-mode VIF wedge respectively.  Routing rebuild-
class config changes around the storm (live intra/GOP apply, or a cold-boot
gate) was developed in PR #123 but is deliberately out of scope for this
cherry-pick.

## 2026-05-19 — Device hang on zoomPct SET (v0.11.0 dev, PR #120)

**Bench:** `root@192.168.1.13` — SSC338Q + IMX335.

**Trigger:** Deployed `feature/zoom-pan-ramp-and-ae-crop` (v0.11.0,
commit 815da64) via `scripts/star6e_direct_deploy.sh cycle`.  Daemon
booted with default config (zoom off), streamed cleanly at 60 fps.
Issued a single live SET:

    /api/v1/set?video0.zoomPct=0.5

HTTP returned `{"ok":true,"data":{"reinit_pending":true}}` as
expected for the MUT_RESTART field.  Within seconds the device
stopped responding to ICMP and SSH.

**Boot-time symptom (pre-hang):** the daemon logged

    WARNING: MI_ISP_CUS3A_SetAECropSize(0,0,1023,1023) failed

once during `pipeline_start`, after `Starting star6e pipeline` and
before `ISP channel ready after 2 ms`.  The full-frame restore call
in `star6e_pan_ramp_start` / `star6e_apply_ae_crop` is firing
*before* CUS3A is enabled.  The SDK rejects it but, more importantly,
the same call path runs from `star6e_pipeline_stop` during the
SIGHUP reinit triggered by the zoom SET — likely interfering with
ISP teardown.

**Suspected root cause:** `star6e_apply_ae_crop` is wired into
`pan_ramp_start` (run during pipeline_start before ISP/CUS3A is up)
and `pipeline_stop` (run during teardown when CUS3A may be in an
intermediate state).  Either:

1. The SDK call to a not-yet-ready ISP corrupts subsequent CUS3A
   init.
2. The teardown-time call races CUS3A disable.

The previous reinit-fragility memory (`venc_star6e_reinit_fragility`)
is also relevant — Star6E in-process reinit has a history of wedging
the SoC under specific conditions.  This may be the AE-crop call
adding a new failure mode on top of that.

**Recovery:** power-cycled.  Re-deployed with the fix below; first
crash mode (boot-time `SetAECropSize` warning) is gone, but **a
second device hang occurred on the second test cycle when zoomPct
was SET back from 0.5 → 0.0** — independent of the AE-crop path
(my new code is bypassed in that branch).  The MUT_RESTART reinit
itself is the wedge.  This matches `venc_star6e_reinit_fragility`
and the `feature/resilience-live-reinit-investigation` memory:
in-process VPE/VENC teardown + recreate has a history of hanging
this BSP.  Resilience already moved to reboot-required in 0.10.15
for the same reason; zoom_pct MUT_RESTART is still on the live-
reinit path.

**Followup tracked separately:** add zoom_pct to the
reboot-required field set (return `{"reboot_required":true}` like
resilience does) — or, with PR #120's smoothing, deprecate
zoom_pct as a runtime knob entirely and pin it to config-only.

**Fix plan (PR #120 followup commit):**

- Defer the first AE-crop call until after CUS3A init completes.
  Probable hook: gated by `g_cus3a_handoff_done` or invoked from the
  cus3a tick (`star6e_pipeline_cus3a_tick`).
- Remove the AE-crop call from `star6e_pipeline_stop` — ISP is
  about to be released anyway, and the next start cold-inits the
  meter.
- Pre-seed `g_star6e_ae_crop_last` to the full-frame value so the
  first idempotent "restore" call is dedup'd away even if it does
  fire.
- Consider gating the entire feature behind a probe: try one
  SetAECropSize at known-good moment; if it returns non-zero,
  disable the feature for the rest of the process lifetime.

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
