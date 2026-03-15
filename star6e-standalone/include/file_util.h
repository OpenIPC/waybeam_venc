#ifndef FILE_UTIL_H
#define FILE_UTIL_H

/** Validate that a path is readable, regular, and non-empty. */
int file_util_validate_regular_file(const char *path, const char *kind,
	const char *log_prefix);

#endif /* FILE_UTIL_H */
