#include <stdio.h>
#include <stdlib.h>

/*
 * Compatibility shim for Maruko uClibc SDK user-space libs when executed
 * under a musl-rootfs target. These symbols are referenced by vendor .so
 * binaries but are not provided by musl with the same names.
 */
FILE* __stdin = NULL;

__attribute__((constructor)) static void maruko_shim_init(void) {
  __stdin = stdin;
}

void __assert(const char* expr, const char* file, int line) {
  fprintf(stderr, "__assert: %s:%d: %s\n", file ? file : "?", line, expr ? expr : "");
  abort();
}

int __fgetc_unlocked(FILE* fp) {
  return fgetc_unlocked(fp);
}

int _MI_PRINT_GetDebugLevel(void) {
  return 0;
}

int backtrace(void** buffer, int size) {
  (void)buffer;
  (void)size;
  return 0;
}

char** backtrace_symbols(void* const* buffer, int size) {
  (void)buffer;
  (void)size;
  return NULL;
}
