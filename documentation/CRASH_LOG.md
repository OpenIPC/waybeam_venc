# Crash Log — Waybeam encoder devices

Per-incident notes on hangs, D-state, and recovery actions on the bench.

## 2026-08-13 — CV610 module unload raced API-respawn successor

**Bench:** `root@192.168.2.181` — Hi3516CV610 + IMX662, 1080p100.

**Build:** issue #220 CV610 backend worktree with the standalone streamer's
inner-ACODEC -> AI -> vendor AENC/Opus path integrated into shared Waybeam RTP
and output helpers.

**Known-good lead-up:** audio initialization completed, including the expected
100 ms ACODEC power-up retry. The daemon sustained about 100 restricted-
low-delay Opus access units/s beside 100 fps H.265 with zero reported audio
drops. An isolated host receiver accepted 12 consecutive RTP v2/PT98 packets;
sequence deltas were all 1 and 48 kHz timestamp deltas were all 480 (10 ms).
This verifies capture/encode activity and wire timing, not acoustic content;
no microphone was connected.

**Trigger:** `/api/v1/set?outgoing.audioPort=5611` performed the expected
process respawn and resumed audio successfully. A subsequent
`/etc/init.d/S95waybeam stop` consulted the stale pidfile created for the
pre-respawn parent. It therefore did not signal the active successor and
called `load-cv610-online stop`, unloading the MPP/audio modules while the
successor still owned the graph. The board became unreachable during that
command. No further device commands were attempted.

**Root cause/fix:** this was not an audio teardown failure: the API respawn had
already completed one full audio teardown/re-init cycle successfully. The
CV610 init script's PID-only stop logic was incompatible with Waybeam's
fork/exec restart model. It now follows the mature init script: detect and
signal `waybeam`, `waybeam-resp`, and `waybeam-wd` by process name, wait until
all are gone, and refuse module unload on timeout.

**State/recovery:** device backups exist as
`/usr/bin/waybeam.pre-audio-20260813`,
`/usr/bin/load-cv610-online.pre-audio-20260813`,
`/etc/init.d/S95waybeam.pre-audio-20260813`, and matching platform/config
backups. The test intended to restore `audio.enabled=false`, audio port 5601,
and `CV610_AUDIO=0`, but the board became unreachable before those writes
could be confirmed. Power-cycle before any further validation, inspect the
three files, restore video-only defaults if necessary, and deploy the fixed
init script before another service stop.

## 2026-08-01 — Craft unreachable during PR #214 unix:// 25 Mbps gate

**Bench:** `root@192.168.2.232` — SSC338Q + IMX335, kernel 4.9.84.

**Build:** PR #214 plus the local cumulative-frame-budget/consumer follow-up,
`/usr/bin/waybeam` SHA-256
`71d8b272a9ed7a9a0e62a9bc909c8596e461e6564f48372991bb755ba3b359ad`,
version 0.63.0 / HTTP contract 0.17.0.

**Known-good lead-up:** target-local abstract AF_UNIX consumer tests at
60 fps passed at 10, 15, and 20 Mbps with zero RTP sequence gaps, zero
`transportDrops`, and no cadence gaps over 17.5 ms. The device queue sysctl
was 1024. `waybeam-link` was gracefully stopped so its rate controller could
not overwrite the test bitrate.

**Trigger:** set `video0.bitrate=25000`, with
`outgoing.server=unix://pr214_venc` and `maxPayloadSize=1400`, then run:

    /tmp/unix_dgram_consumer_pr214 --name pr214_venc --duration 8

The first 25 Mbps measurement ended after its `READY` line without a final
JSON summary, but a bounded liveness check immediately afterward succeeded:
venc was still encoding at 59–60 fps / 23.6–25.5 Mbps and HTTP reported
`transportDrops=0`. A standalone retry printed `READY`, then its SSH session
failed with `Timeout, server 192.168.2.232 not responding.` before the
eight-second result. No further device commands were attempted, per the
device-unresponsive recovery rule.

**State at stop:** `/etc/waybeam.json` still selected `unix://pr214_venc`,
25 Mbps, 60 fps; `waybeam-link` was stopped. The exact pre-test configuration
was backed up at `/root/waybeam.json.pr214-live-start`; pre-deploy
binary/init/config backups are under `/root/*pr214-pretest`. No dmesg could be
collected immediately after the timeout without violating the no-retry rule.

**Recovery:** wait for human confirmation that the craft has recovered or
power-cycle it. Then restore the latest pre-test state without SIGKILL:

    cp /root/waybeam.json.pr214-live-start /etc/waybeam.json
    /etc/init.d/S95waybeam restart
    /etc/init.d/S96waybeam-link start

Confirm `outgoing.server=frame-shm://venc_frame`, HTTP health, and dmesg before
resuming. Root cause is unresolved: this observation alone cannot distinguish
a craft/SSH outage from an encoder or kernel stall.

**Recovery completed:** after a human power cycle, the saved live-start JSON
was restored and both `S95waybeam` and `S96waybeam-link` were started
gracefully. HTTP reported 100 fps and `frame-shm://venc_frame`; the frame
counter advanced from 1 to 774 with zero transport drops, and waybeam-link
attached to `venc_frame`. Post-boot dmesg did contain MI/CMDQ trigger-timeout
warnings, so no further PR #214 device gates were attempted. The user then
rebooted the craft and removed it from this validation session.

**Follow-up — 2026-08-02:** after another user-confirmed reboot, the remaining
Star6E V2 and V4–V9 gates completed without the connectivity failure
recurring. The craft sustained 25 Mbps at both 60 and 120 fps, including an
authoritative 0.888 ms maximum sender-side Unix frame spread at 120 fps. A
five-second wedged-consumer test reached 100% queue fill and recovered without
encoder or watchdog errors. Repeated live redirect and UDP/SHM/audio tests
also passed. The incident's root cause remains unknown; the successful rerun
shows it is not a reproducible consequence of the 25 Mbps Unix workload.
Production was restored byte-for-byte afterward, both services were started
normally, and the frame-SHM counter advanced from 2,275 to 2,576 with no new
drops. Current dmesg had MI teardown warnings from the repeated controlled
restart cycles but no timeout, hung-task, oops, panic, or call-trace entry.

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
D-state wedge and the cross-mode VIF wedge respectively.

**SUPERSEDED (2026-06-07):** the MMU storm above is now **fixed** (the
`debug_osd_destroy` settle, PR #126) — it did not fire on either the respawn
or the in-process path in the latest bench runs.  The "in-process rebuild
storms too / intrinsic, independent of process model" claim was about the MMU
storm specifically; with that fixed, the residual single-PID blocker is a
different one (kernel VIF/VPE/ISP channel state that only execv releases).
Full retest and the conclusion that fork+exec respawn is a driver requirement:
see `documentation/STAR6E_SINGLE_PID_REINIT_FINDINGS.md`.

Routing rebuild-
class config changes around the storm (live intra/GOP apply, or a cold-boot
gate) was developed in PR #123 but is deliberately out of scope for this
cherry-pick.

## 2026-05-20 — Stab teardown wedge RESOLVED by HW-crop refactor

**Bench:** `root@192.168.1.13` — SSC338Q + IMX335.

**Resolution of the wedge logged below.** Root cause was specific to the
legacy stab data path: VPE port0 ran at full src dim, **unbound**, and was
drained manually by the stab thread (the *only* consumer).  `pipeline_stop`
stopped that thread before unbinding VIF→VPE, so for the >150 ms teardown
window the source kept filling an un-consumed depth-8 port0 queue →
`[vpe0_P0_MAIN]` wedged in D-state.  At full 1080p that queue overflows in
~130 ms.  (A naive "unbind VIF→VPE first" band-aid traded this for a
GetBuf-vs-UnBind lock contention and still wedged on its first teardown.)

**Fix:** HW-crop refactor — port0 hardware-crops the stab window via
`MI_VPE_SetPortCrop` straight into a VENC **bind** (consumed by hardware, no
manual drain, torn down by the standard bound-port path); only a tiny port1
256×256 detector tap is manually drained, disabled on stop via a pause/park
quiesce handshake so `DisablePort(0,1)` never races a GetBuf.  **Validated
2026-05-20: 5/5 consecutive `S95waybeam restart` cycles clean** (uptime
continuous, no watchdog reboot), HW mode engaged, 60 fps.  Note `[vpe0_P0_MAIN]`
sitting in `DW` during streaming is normal on this SoC (all modes) and inflates
load average — it is NOT the wedge; the wedge is the load climbing while the
SoC goes unreachable + watchdog reset.

## 2026-05-20 — Repeated teardown wedges → hard hang (PR #122 DIS stab perf work)

**Bench:** `root@192.168.1.13` — SSC338Q + IMX335.

**Context:** Validating PR #122 (digital image stabilization) and then a
perf fix (Shift_Detector throttle to every 2nd frame + crop 384→256, box
256→128, pyramid 3→2). Each `stabCropPct` change is `MUT_RESTART`, so each
test needs a `/etc/init.d/S95waybeam restart`.

**Symptom:** The stab teardown path intermittently wedges `[vpe0_P0_MAIN]`
into D-state on `restart` (stop+start). Of ~5 new-binary restarts this
session, T1 / cycle-A / cycle-B were clean; the T4 (dual+stab) and a plain
`stabCropPct 80→0` restart each wedged → SoC unreachable ~30–80s → watchdog
(`panic=20`) reset, recovering to a healthy boot. No venc/MI oops survives
the reset; the only post-boot dmesg fault is the unrelated 8812eu wifi
`DEBUG_LOCKS_WARN_ON(in_interrupt())`.

**Hard hang:** After ~8 wedge/reset cycles in one session, a restart of the
throttle+cheapen build (md5 e28796a1, stab=80) wedged and did **not**
self-recover — >150 s unreachable, ARP `FAILED`. Watchdog did not fire (or
could not reset the SoC). Cumulative SoC-state degradation across repeated
wedges is the suspected cause. **Requires a physical power cycle.**

**State at hang:** new binary already renamed into `/usr/bin/waybeam` (will
run on next boot via S95waybeam); `/etc/waybeam.json` has `stabCropPct=80`.
Backups on device: `/usr/bin/waybeam.old.bak`, `/etc/waybeam.json.prepr122`.

**Recovery procedure:** power-cycle, let it boot (auto-starts the new binary
at stab=80), confirm `grep 'stab: src=' /tmp/waybeam.log` and the fps line.
The throttle+cheapen fps measurement is still pending (could not capture
before the hang). Class: known venc teardown fragility — see
[[venc_teardown_regression]] / [[venc_star6e_reinit_fragility]], not a
regression introduced by the stab perf change.

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
