#ifndef STAR6E_RUNTIME_H
#define STAR6E_RUNTIME_H

#include "backend.h"

/* Comm names for child forks created by the Star6E runtime.  Both must
 * differ from "venc" — that is what is_another_venc_running() in main.c
 * matches against, and a duplicate match would abort SIGHUP-respawn with
 * "venc already running... exiting". */
#define VENC_COMM_WATCHDOG  "venc-wd"
#define VENC_COMM_RESPAWN   "venc-resp"

/* Path constants for the SIGHUP-respawn flow. */
#define VENC_SELF_EXE_PATH  "/proc/self/exe"
#define VENC_LOG_PATH       "/tmp/venc.log"

/** Return the Star6E backend operations table. */
const BackendOps *star6e_runtime_backend_ops(void);

/** Return non-zero if a SIGHUP / /api/v1/restart asked for a process-
 *  level restart.  main() calls this after backend teardown finishes
 *  and, if set, calls star6e_runtime_respawn_after_exit() before
 *  returning. */
int star6e_runtime_respawn_pending(void);

/** Fork a child that execv's a fresh /proc/self/exe and return.  Caller
 *  exits the process; the child sleeps briefly to let the kernel reap
 *  the parent and release per-pid SDK state, then becomes the new venc
 *  with a brand-new PID and zero residual MI state. */
void star6e_runtime_respawn_after_exit(void);

#endif /* STAR6E_RUNTIME_H */
