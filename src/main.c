#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "backend.h"
#include "venc_respawn.h"

/* Single-instance gate.  We walk /proc and reject startup if any
 * other userspace process has comm "waybeam".  An earlier flock-based
 * pidfile lock was tried as belt-and-suspenders against the TOCTOU
 * window here, but on Star6E the SIGHUP-respawn handoff hit a kernel
 * race where the new image's flock() saw the OFD as still locked past
 * 600 ms after parent reap, with no /proc/PID/fd entry referencing
 * the file.  The /proc walk on its own is sufficient: the only path
 * to a second instance is an external `/usr/bin/waybeam &` racing the
 * init script, and the TOCTOU window between scan and process-exit
 * is irrelevant because both processes would notice each other on
 * the walk anyway.  Comm is pinned via prctl(PR_SET_NAME) early in
 * main so the SIGHUP-respawn child (whose comm is "waybeam-resp"
 * until execv) is correctly distinguished from a fully-running peer. */

static int is_another_waybeam_running(void)
{
  pid_t my_pid = getpid();
  DIR* proc = opendir("/proc");
  if (!proc) {
    return 0;
  }

  struct dirent* ent;
  while ((ent = readdir(proc)) != NULL) {
    /* Skip non-numeric entries */
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
      continue;
    }

    long pid = strtol(ent->d_name, NULL, 10);
    if (pid <= 0 || pid == (long)my_pid) {
      continue;
    }

    /* Skip kernel threads (empty /proc/PID/cmdline).  Prevents a stale
     * "[waybeam]" MI_VENC kernel worker — left behind when MI_SYS_Exit
     * is bypassed by SIGKILL/OOM/panic — from blocking restart until
     * reboot.  Userspace processes always have argv[0]\0 in cmdline. */
    char cmdline_path[64];
    snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%ld/cmdline", pid);
    FILE* cf = fopen(cmdline_path, "r");
    if (!cf) {
      continue;
    }
    int cmdline_empty = (fgetc(cf) == EOF);
    fclose(cf);
    if (cmdline_empty) {
      continue;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
    FILE* f = fopen(path, "r");
    if (!f) {
      continue;
    }

    char comm[32] = {0};
    if (fgets(comm, sizeof(comm), f)) {
      /* Strip trailing newline */
      size_t len = strlen(comm);
      if (len > 0 && comm[len - 1] == '\n') {
        comm[len - 1] = '\0';
      }
      if (strcmp(comm, "waybeam") == 0) {
        fclose(f);
        closedir(proc);
        return 1;
      }
    }
    fclose(f);
  }

  closedir(proc);
  return 0;
}

int main(int argc, char* argv[])
{
	const BackendOps *backend;

  (void)argc;
  (void)argv;

  /* Pin /proc/self/comm to "waybeam" before is_another_waybeam_running().
   * The kernel derives comm from basename(argv[0]) on execve, so the
   * normal init-script start (/usr/bin/waybeam) and the SIGHUP-respawn
   * execv (argv[0]="waybeam" — see star6e_runtime_respawn_after_exit)
   * already produce comm="waybeam".  This prctl is defensive belt-and-
   * suspenders: it guarantees the scanner key regardless of what argv[0]
   * an external caller decides to pass. */
  (void)prctl(PR_SET_NAME, "waybeam", 0, 0, 0);

  if (is_another_waybeam_running()) {
    printf("waybeam already running... exiting...\n");
    return 1;
  }

	backend = backend_get_selected();
	if (!backend || !backend->name) {
		fprintf(stderr, "ERROR: No backend compiled into this build.\n");
		return 1;
	}

	printf("> SoC backend build: %s\n", backend->name);
	int rc = backend_execute(backend);

	/* SIGHUP / /api/v1/restart / resilience ref_* delta exits cleanly
	 * here and forks a successor process for a true cold restart.
	 * Shared between both backends — both BSPs have the same
	 * limitation: in-process MI_SYS_Exit + MI_SYS_Init does not yield
	 * a clean kernel state.  See include/venc_respawn.h. */
	if (venc_respawn_pending())
		venc_respawn_after_exit();

	return rc;
}
