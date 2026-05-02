#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "backend.h"
#if defined(PLATFORM_STAR6E) && !defined(PLATFORM_MARUKO)
#include "star6e_runtime.h"
#endif

/* Race-free single-instance gate.  open() + flock(LOCK_EX|LOCK_NB) is
 * atomic from the kernel's perspective — two venc starting back-to-back
 * cannot both win the lock.  The /proc walk below stays as a fallback
 * for the (rare) case where the pidfile filesystem is unavailable, so
 * loss of /tmp does not silently disable the gate.
 *
 * O_CLOEXEC: the SIGHUP respawn path execv's /proc/self/exe, and the
 * new image must be able to re-acquire the lock — close-on-exec drops
 * the inherited fd so the kernel releases the BSD lock, then the new
 * image's own acquire_pidfile_lock() takes it again.  Without
 * O_CLOEXEC the inherited locked fd would shadow the new image's
 * flock() attempt and trip the "already running" branch on every
 * legitimate respawn.
 *
 * The fd is intentionally leaked: closing it would release the lock,
 * and process exit reclaims it.  No close() is needed. */
static int acquire_pidfile_lock(void)
{
  static const char *paths[] = { "/var/run/venc.pid", "/tmp/venc.pid" };
  int fd = -1;
  const char *path = NULL;

  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    fd = open(paths[i], O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd >= 0) {
      path = paths[i];
      break;
    }
  }
  if (fd < 0) {
    fprintf(stderr, "[main] cannot open venc.pid: %s\n", strerror(errno));
    return -1;
  }

  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int e = errno;
    close(fd);
    if (e == EWOULDBLOCK)
      return -2;
    fprintf(stderr, "[main] flock(%s) failed: %s\n", path, strerror(e));
    return -1;
  }

  /* Write our pid for observability — informational only; the lock
   * itself is the authoritative gate. */
  char buf[16];
  int n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
  if (n > 0) {
    (void)ftruncate(fd, 0);
    (void)write(fd, buf, (size_t)n);
  }
  return fd;
}

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

  /* Pin /proc/self/comm to "venc" before is_another_venc_running().  The
   * SIGHUP-respawn path execv's /proc/self/exe, which makes the kernel
   * derive comm from the symlink basename ("exe") instead of argv[0].
   * Without this rename, is_another_venc_running() (which matches comm)
   * silently fails to detect a running respawned instance, and an
   * externally-launched second venc could start a duplicate. */
  (void)prctl(PR_SET_NAME, "venc", 0, 0, 0);

  /* Primary gate: race-free pidfile + flock.  Fallback: legacy /proc
   * scan, which is TOCTOU-prone but still valuable when the pidfile
   * filesystem is unavailable (the open() / flock() failure path
   * returns -1, distinct from -2 = lock contention). */
  int pidfile_fd = acquire_pidfile_lock();
  if (pidfile_fd == -2) {
    printf("venc already running (pidfile lock held)... exiting...\n");
    return 1;
  }
  if (pidfile_fd < 0 && is_another_venc_running()) {
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
