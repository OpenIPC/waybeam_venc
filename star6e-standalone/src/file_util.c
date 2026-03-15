#include "file_util.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int file_util_validate_regular_file(const char *path, const char *kind,
	const char *log_prefix)
{
	struct stat st;
	const char *label = kind ? kind : "file";
	const char *prefix = log_prefix ? log_prefix : "";

	if (!path || !*path) {
		return -1;
	}
	if (stat(path, &st) != 0 || access(path, R_OK) != 0) {
		fprintf(stderr, "ERROR: %s%s is not readable: %s (errno=%d)\n",
			prefix, label, path, errno);
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "ERROR: %s%s is not a regular file: %s\n",
			prefix, label, path);
		return -1;
	}
	if (st.st_size <= 0) {
		fprintf(stderr, "ERROR: %s%s is empty: %s\n", prefix, label, path);
		return -1;
	}
	return 0;
}
