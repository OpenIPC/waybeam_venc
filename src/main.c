#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#if defined(PLATFORM_STAR6E) && !defined(PLATFORM_MARUKO)
#include "star6e_runtime.h"
#endif

static int is_another_venc_running(void)
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
     * "[venc]" MI_VENC kernel worker — left behind when MI_SYS_Exit is
     * bypassed by SIGKILL/OOM/panic — from blocking restart until
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
      if (strcmp(comm, "venc") == 0) {
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

  if (is_another_venc_running()) {
    printf("venc already running... exiting...\n");
    return 1;
  }

	backend = backend_get_selected();
	if (!backend || !backend->name) {
		fprintf(stderr, "ERROR: No backend compiled into this build.\n");
		return 1;
	}

	printf("> SoC backend build: %s\n", backend->name);
	int rc = backend_execute(backend);

#if defined(PLATFORM_STAR6E) && !defined(PLATFORM_MARUKO)
	/* SIGHUP / /api/v1/restart on Star6E exits cleanly here and forks
	 * a successor process for true cold restart.  See
	 * star6e_runtime_respawn_after_exit() for the rationale —
	 * in-process MI_SYS_Exit + MI_SYS_Init is broken on this BSP. */
	if (star6e_runtime_respawn_pending())
		star6e_runtime_respawn_after_exit();
#endif

	return rc;
}
