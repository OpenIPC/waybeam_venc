#ifndef STAR6E_RUNTIME_H
#define STAR6E_RUNTIME_H

#include "backend.h"

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
