#include "venc_respawn.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Comm name for the respawn child.  Must differ from "waybeam" — that
 * is what is_another_waybeam_running() in main.c matches against, and
 * a duplicate match would abort the respawn with the "already running"
 * banner. */
#define VENC_COMM_RESPAWN   "waybeam-resp"

/* Re-exec the same binary image as the current process. */
#define VENC_SELF_EXE_PATH  "/proc/self/exe"

/* Stdout/stderr destination for the child between dup2 and execve. */
#define VENC_LOG_PATH       "/tmp/waybeam.log"

static int g_respawn_pending;
static int g_respawn_cold_vif;

void venc_respawn_request(void)
{
	g_respawn_pending = 1;
}

void venc_respawn_set_cold_vif(int enable)
{
	g_respawn_cold_vif = enable ? 1 : 0;
}

int venc_respawn_pending(void)
{
	return g_respawn_pending;
}

void venc_respawn_after_exit(void)
{
	/* Capture our PID BEFORE fork.  The child cannot use getppid()
	 * to learn it: by the time the child first runs, the parent may
	 * already have exited (we are about to return into main, which
	 * exits immediately), so the child gets reparented to init/
	 * subreaper and getppid() returns 1.  COW inheritance carries
	 * this static across fork verbatim. */
	pid_t parent_pid = getpid();
	pid_t child = fork();

	if (child < 0) {
		fprintf(stderr,
			"ERROR: fork() for respawn failed: %s — process exiting\n",
			strerror(errno));
		return;
	}

	if (child > 0) {
		fprintf(stderr,
			"> Respawning: parent %d → child %d\n",
			(int)getpid(), (int)child);
		return;
	}

	(void)prctl(PR_SET_NAME, VENC_COMM_RESPAWN, 0, 0, 0);

	/* Wait for the parent to ACTUALLY exit before we exec.  The previous
	 * heuristic (getppid() != 1) is unreliable under procd / systemd /
	 * any process that calls PR_SET_CHILD_SUBREAPER: when the parent
	 * dies, we reparent to the subreaper instead of init, so getppid()
	 * never reaches 1 and the loop times out while the parent is still
	 * alive — the new image then runs while the old SDK state is
	 * still owned by the dying parent.
	 *
	 * Polling kill(parent_pid, 0) for ESRCH is the authoritative test:
	 * it reports the actual death of that specific PID regardless of
	 * who the new parent is.  Cap raised to 30 s because MI_SYS_Exit
	 * can stall in D-state on Star6E during teardown (observed: ~9
	 * load-avg with the pipeline draining).  On timeout, abort the
	 * exec rather than racing live SDK state — silently losing the
	 * SIGHUP is better than two venc's stomping on each other. */
	int waited_ms = 0;
	const int wait_cap_ms = 30 * 1000;
	while (waited_ms < wait_cap_ms) {
		if (kill(parent_pid, 0) == -1 && errno == ESRCH)
			break;
		usleep(100 * 1000);
		waited_ms += 100;
	}
	fprintf(stderr,
		"> Respawn child: parent pid %d gone after %d ms, proceeding to exec\n",
		(int)parent_pid, waited_ms);
	if (waited_ms >= wait_cap_ms) {
		/* stderr is unbuffered and still wired to the parent's log
		 * sink at this point — emit before the dup2 swap so the
		 * message lands in the same /tmp/waybeam.log tail an
		 * operator will check. */
		fprintf(stderr,
			"ERROR: respawn child timed out after %d s waiting "
			"for parent pid %d to exit — aborting exec to avoid "
			"racing live SDK state.  Parent likely stuck in "
			"MI_SYS teardown; reboot may be required.\n",
			wait_cap_ms / 1000, (int)parent_pid);
		_exit(1);
	}

	/* Additional settle delay after parent-PID exit, before execv.
	 *
	 * Parent-PID death (kill ESRCH) only confirms the userspace
	 * process is gone — the kernel-resident MI SDK driver state
	 * (MI_VENC channel teardown, DMA drain, sensor mode release)
	 * may still be in flight.  Executing a fresh waybeam against
	 * half-released kernel state can panic the SoC on
	 * refPred-active → refPred-inactive transitions (e.g.
	 * `resilience=fpv` → `resilience=endurance` deactivates
	 * SetRefParam, which triggers significant driver state
	 * unwind).  500 ms is empirically enough on imx335 @ 1080p120
	 * to let the driver complete its post-exit cleanup before the
	 * fresh MI_SYS_Init lands. */
	usleep(500 * 1000);

	sigset_t empty;
	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);

	/* Re-route stdout/stderr to /tmp/waybeam.log in append mode so the
	 * pre-respawn tail stays available for diagnosis.  If dup2 fails
	 * (e.g. fd table exhaustion from a leaked descriptor we missed in
	 * teardown) bail before execv — running blind would lose any panic
	 * output the fresh venc emits. */
	int log = open(VENC_LOG_PATH,
		O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (log >= 0) {
		if (dup2(log, STDOUT_FILENO) < 0 ||
		    dup2(log, STDERR_FILENO) < 0)
			_exit(127);
		if (log > 2)
			close(log);
	}

	/* Scrub all non-stdio fds before execv: the SigmaStar SDK opens
	 * /dev/mi_poll (and a few other internal fds) without CLOEXEC, and
	 * MI_SYS_Exit does not release every one of them.  Without this
	 * scrub, every respawn cycle leaks 1+ mi_poll fds; a long-running
	 * device hits the per-process fd cap eventually.  Source-level
	 * O_CLOEXEC on our own opens (sockets, pipes, recorder, IMU,
	 * config, /dev/null) is still in effect and makes this loop a
	 * narrow safety net rather than the primary defence. */
	{
		DIR *d = opendir("/proc/self/fd");
		if (d) {
			int dfd_keep = dirfd(d);
			struct dirent *e;
			int closed_count = 0;
			int skipped_count = 0;
			while ((e = readdir(d)) != NULL) {
				if (!isdigit((unsigned char)e->d_name[0]))
					continue;
				int fd = atoi(e->d_name);
				if (fd <= STDERR_FILENO || fd == dfd_keep)
					continue;
				/* Bench-localized 2026-05-19 (PR #120): closing
				 * /dev/mi_sys (and likely other /dev/mi_* SDK
				 * descriptors) here deadlocks the SigmaStar
				 * driver after a zoom_pct MUT_RESTART teardown.
				 * MI_SYS_Exit returns cleanly but does not
				 * release its kernel-side state for the fd, so
				 * the close() syscall reaches the driver's
				 * release handler in a half-torn-down state and
				 * hangs uninterruptibly.  Watchdog escalation
				 * then sysrq-b's the box.
				 *
				 * Skip /dev/mi_* fds — let them be inherited
				 * into the exec'd image as orphans.  This
				 * reintroduces a slow ~1-fd-per-respawn leak
				 * (the original concern this scrub was for),
				 * bounded by RLIMIT_NOFILE (1024); long-running
				 * operational concern, vastly preferable to
				 * wedging the SoC. */
				char link_path[64];
				char target[256] = {0};
				snprintf(link_path, sizeof(link_path),
					"/proc/self/fd/%d", fd);
				ssize_t n = readlink(link_path, target,
					sizeof(target) - 1);
				if (n < 0) n = 0;
				target[n] = '\0';
				if (strncmp(target, "/dev/mi_", 8) == 0) {
					/* Cold-init exception: on a sensor-mode
					 * change (video0.size), the inherited VIF
					 * and VPE fds pin the OLD mode's kernel
					 * state — re-initing them to a new mode in
					 * the fresh process wedges vpe0_P0_MAIN.
					 * Close just those two so they re-init
					 * cold.  /dev/mi_sys and the rest stay
					 * inherited (closing mi_sys here is the
					 * confirmed deadlock).  Gated on
					 * g_respawn_cold_vif so it never runs on a
					 * same-mode respawn. */
					if (g_respawn_cold_vif &&
					    (strcmp(target, "/dev/mi_vif") == 0 ||
					     strcmp(target, "/dev/mi_vpe") == 0)) {
						fprintf(stderr,
							"[respawn] cold-init: closing %s\n",
							target);
						close(fd);
						closed_count++;
						continue;
					}
					skipped_count++;
					continue;
				}
				close(fd);
				closed_count++;
			}
			fprintf(stderr,
				"[respawn] fd-scrub: closed=%d skipped=%d (SDK /dev/mi_*)\n",
				closed_count, skipped_count); fflush(stderr);
			closedir(d);
		}
	}

	/* argv is intentionally minimal — main.c currently ignores argv, so
	 * any future flag (e.g. `-c <path>`) added without saving the parent's
	 * argv into a static and replaying it here would silently disappear
	 * across respawn. */
	char *args[] = { (char *)"waybeam", NULL };
	execv(VENC_SELF_EXE_PATH, args);
	/* exec failed — fall through and exit so init/operator notices */
	_exit(127);
}
