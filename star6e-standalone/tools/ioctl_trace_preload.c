#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FD_TRACK_MAX 1024
#define PATH_LEN 96

static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_fd_path[FD_TRACK_MAX][PATH_LEN];
static __thread int g_in_hook = 0;

static void trace_log(const char* fmt, ...) {
  if (g_in_hook) {
    return;
  }
  g_in_hook = 1;

  char line[512];
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  int off = snprintf(line, sizeof(line), "[ioctl-trace %ld.%03ld pid=%d] ",
    (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000L), (int)getpid());
  if (off < 0 || off >= (int)sizeof(line)) {
    g_in_hook = 0;
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
  va_end(ap);
  if (n < 0) {
    g_in_hook = 0;
    return;
  }

  size_t len = strnlen(line, sizeof(line));
  if (len + 1 < sizeof(line)) {
    line[len++] = '\n';
  }
  (void)write(STDERR_FILENO, line, len);
  g_in_hook = 0;
}

static bool should_track_path(const char* path) {
  if (!path) {
    return false;
  }
  if (strstr(path, "/dev/mi_") != NULL) {
    return true;
  }
  if (strstr(path, "/dev/mi") != NULL) {
    return true;
  }
  return false;
}

static void track_fd_path(int fd, const char* path) {
  if (fd < 0 || fd >= FD_TRACK_MAX || !path) {
    return;
  }
  pthread_mutex_lock(&g_fd_lock);
  snprintf(g_fd_path[fd], PATH_LEN, "%s", path);
  pthread_mutex_unlock(&g_fd_lock);
}

static void clear_fd_path(int fd) {
  if (fd < 0 || fd >= FD_TRACK_MAX) {
    return;
  }
  pthread_mutex_lock(&g_fd_lock);
  g_fd_path[fd][0] = '\0';
  pthread_mutex_unlock(&g_fd_lock);
}

static bool get_fd_path(int fd, char out[PATH_LEN]) {
  if (!out || fd < 0 || fd >= FD_TRACK_MAX) {
    return false;
  }
  bool ok = false;
  pthread_mutex_lock(&g_fd_lock);
  if (g_fd_path[fd][0] != '\0') {
    snprintf(out, PATH_LEN, "%s", g_fd_path[fd]);
    ok = true;
  }
  pthread_mutex_unlock(&g_fd_lock);
  return ok;
}

int open(const char* path, int flags, ...) {
  static int (*real_open)(const char*, int, ...) = NULL;
  if (!real_open) {
    real_open = (int (*)(const char*, int, ...))dlsym(RTLD_NEXT, "open");
  }
  if (!real_open) {
    errno = ENOSYS;
    return -1;
  }

  mode_t mode = 0;
  int need_mode = (flags & O_CREAT) != 0;
  if (need_mode) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  int fd = need_mode ? real_open(path, flags, mode) : real_open(path, flags);
  if (fd >= 0 && should_track_path(path)) {
    track_fd_path(fd, path);
    trace_log("open(\"%s\",0x%x) -> fd=%d", path, flags, fd);
  }
  return fd;
}

int openat(int dirfd, const char* path, int flags, ...) {
  static int (*real_openat)(int, const char*, int, ...) = NULL;
  if (!real_openat) {
    real_openat = (int (*)(int, const char*, int, ...))dlsym(RTLD_NEXT, "openat");
  }
  if (!real_openat) {
    errno = ENOSYS;
    return -1;
  }

  mode_t mode = 0;
  int need_mode = (flags & O_CREAT) != 0;
  if (need_mode) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  int fd = need_mode
    ? real_openat(dirfd, path, flags, mode)
    : real_openat(dirfd, path, flags);
  if (fd >= 0 && should_track_path(path)) {
    track_fd_path(fd, path);
    trace_log("openat(dirfd=%d,\"%s\",0x%x) -> fd=%d", dirfd, path, flags, fd);
  }
  return fd;
}

int close(int fd) {
  static int (*real_close)(int) = NULL;
  if (!real_close) {
    real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
  }
  if (!real_close) {
    errno = ENOSYS;
    return -1;
  }

  char path[PATH_LEN] = {0};
  bool tracked = get_fd_path(fd, path);
  int ret = real_close(fd);
  if (tracked) {
    trace_log("close(fd=%d path=%s) -> %d", fd, path, ret);
    clear_fd_path(fd);
  }
  return ret;
}

int ioctl(int fd, unsigned long req, ...) {
  static int (*real_ioctl)(int, unsigned long, ...) = NULL;
  if (!real_ioctl) {
    real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
  }
  if (!real_ioctl) {
    errno = ENOSYS;
    return -1;
  }

  va_list ap;
  va_start(ap, req);
  void* argp = va_arg(ap, void*);
  va_end(ap);

  char path[PATH_LEN] = {0};
  bool tracked = get_fd_path(fd, path);
  if (tracked) {
    uint32_t a0 = 0;
    uint32_t a1 = 0;
    uint32_t a2 = 0;
    uint32_t a3 = 0;
    uint32_t a4 = 0;
    uint32_t a5 = 0;
    if (argp) {
      memcpy(&a0, argp, sizeof(a0));
      memcpy(&a1, (uint8_t*)argp + sizeof(a0), sizeof(a1));
      memcpy(&a2, (uint8_t*)argp + sizeof(a0) * 2, sizeof(a2));
      memcpy(&a3, (uint8_t*)argp + sizeof(a0) * 3, sizeof(a3));
      memcpy(&a4, (uint8_t*)argp + sizeof(a0) * 4, sizeof(a4));
      memcpy(&a5, (uint8_t*)argp + sizeof(a0) * 5, sizeof(a5));
    }
    trace_log("ioctl-pre(fd=%d path=%s req=0x%08lx arg=%p "
              "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x a5=0x%08x)",
      fd, path, req, argp, a0, a1, a2, a3, a4, a5);
  }

  int ret = real_ioctl(fd, req, argp);

  if (tracked) {
    uint32_t a0 = 0;
    uint32_t a1 = 0;
    uint32_t a2 = 0;
    uint32_t a3 = 0;
    uint32_t a4 = 0;
    uint32_t a5 = 0;
    if (argp) {
      memcpy(&a0, argp, sizeof(a0));
      memcpy(&a1, (uint8_t*)argp + sizeof(a0), sizeof(a1));
      memcpy(&a2, (uint8_t*)argp + sizeof(a0) * 2, sizeof(a2));
      memcpy(&a3, (uint8_t*)argp + sizeof(a0) * 3, sizeof(a3));
      memcpy(&a4, (uint8_t*)argp + sizeof(a0) * 4, sizeof(a4));
      memcpy(&a5, (uint8_t*)argp + sizeof(a0) * 5, sizeof(a5));
    }
    trace_log("ioctl(fd=%d path=%s req=0x%08lx arg=%p "
              "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x a5=0x%08x) -> %d errno=%d",
      fd, path, req, argp, a0, a1, a2, a3, a4, a5, ret, (ret < 0) ? errno : 0);

    if (req == 0xc014690f && a2 > 0x1000 && a4 > 0 && a4 <= 32) {
      uint32_t d0 = 0;
      uint32_t d1 = 0;
      uint32_t d2 = 0;
      uint32_t d3 = 0;
      void* p = (void*)(uintptr_t)a2;
      memcpy(&d0, p, sizeof(d0));
      if (a4 >= 8) memcpy(&d1, (uint8_t*)p + 4, sizeof(d1));
      if (a4 >= 12) memcpy(&d2, (uint8_t*)p + 8, sizeof(d2));
      if (a4 >= 16) memcpy(&d3, (uint8_t*)p + 12, sizeof(d3));
      trace_log("  cust-payload ptr=%p size=%u d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x",
        p, a4, d0, d1, d2, d3);
    }
  }

  return ret;
}

__attribute__((constructor))
static void ioctl_trace_ctor(void) {
  trace_log("ioctl preload trace active");
}
