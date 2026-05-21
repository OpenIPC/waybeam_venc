#ifndef VENC_RESPAWN_H
#define VENC_RESPAWN_H

/* Process-level respawn helper, shared between Star6E and Maruko
 * backends.  Both BSPs have the same fundamental limitation:
 * in-process MI_SYS_Exit + MI_SYS_Init does not yield a clean
 * kernel-side SDK state — the driver retains "already_inited"
 * flags tied to the PID, and on Maruko the teardown path can hit
 * a page fault inside MI_SYS_IMPL_FlushInputPortTasks.
 *
 * The only reliable cold restart is a *new PID*.  Pattern:
 *
 *   1. Runtime's reinit handler calls venc_respawn_request()
 *      and exits the run loop.
 *   2. main() invokes backend->teardown (clean MI_SYS_Exit).
 *   3. main() checks venc_respawn_pending() and, if set, calls
 *      venc_respawn_after_exit() which forks; the parent returns
 *      and main() exits, the child waits for the parent PID to
 *      actually die, then execv's /proc/self/exe.  Forking AFTER
 *      teardown is critical — if we forked before, the child
 *      would inherit MI device fds and the kernel's per-pid
 *      cleanup wouldn't fully fire when the parent calls
 *      MI_SYS_Exit.
 *
 * Bench-validated on Star6E against 12 consecutive cross-mode
 * sensor SIGHUPs with no degradation.  Originally lived in
 * star6e_runtime.c — lifted here so Maruko can use the same
 * machinery for ref_* preset deltas that the in-process reinit
 * cannot survive. */

/** Request a process-level respawn after current backend teardown
 *  completes.  Latched flag — call sites do not need to coordinate. */
void venc_respawn_request(void);

/** Returns non-zero iff venc_respawn_request() has been called
 *  since process start. */
int venc_respawn_pending(void);

/** Request that the next respawn cold-init VIF/VPE: the fd-scrub will
 *  additionally close the inherited /dev/mi_vif and /dev/mi_vpe fds so the
 *  fresh process re-inits them from a clean kernel state.  Set this ONLY for
 *  a sensor-mode change (video0.size change) — the close() is in the same
 *  driver-release path that deadlocks /dev/mi_sys, so it must not run on
 *  every respawn.  Latched until the next respawn. */
void venc_respawn_set_cold_vif(int enable);

/** Fork a child that waits for the parent to exit, then execv's
 *  /proc/self/exe.  Caller MUST exit the process immediately after
 *  this returns from the parent path (it is a non-blocking fork). */
void venc_respawn_after_exit(void);

#endif /* VENC_RESPAWN_H */
