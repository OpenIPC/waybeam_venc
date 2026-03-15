#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file_util.h"
#include "test_helpers.h"

static int create_temp_file(char *path_template, const char *contents)
{
	int fd = mkstemp(path_template);
	if (fd < 0) {
		return -1;
	}

	if (contents && *contents) {
		size_t len = strlen(contents);
		if (write(fd, contents, len) != (ssize_t)len) {
			close(fd);
			unlink(path_template);
			return -1;
		}
	}

	close(fd);
	return 0;
}

int test_file_util(void)
{
	int failures = 0;
	char file_path[] = "/tmp/test_file_util_regular_XXXXXX";
	char empty_path[] = "/tmp/test_file_util_empty_XXXXXX";
	char dir_path[] = "/tmp/test_file_util_dir_XXXXXX";
	char missing_path[] = "/tmp/test_file_util_missing_XXXXXX";

	CHECK("file_null_path",
		file_util_validate_regular_file(NULL, "test", "") != 0);

	CHECK("file_create_regular",
		create_temp_file(file_path, "payload") == 0);
	CHECK("file_regular_ok",
		file_util_validate_regular_file(file_path, "test", "") == 0);

	CHECK("file_create_empty",
		create_temp_file(empty_path, NULL) == 0);
	CHECK("file_empty_rejected",
		file_util_validate_regular_file(empty_path, "test", "") != 0);

	CHECK("file_create_missing_seed",
		create_temp_file(missing_path, "seed") == 0);
	CHECK("file_remove_missing_seed",
		unlink(missing_path) == 0);
	CHECK("file_missing_rejected",
		file_util_validate_regular_file(missing_path, "test", "") != 0);

	CHECK("file_make_dir",
		mkdtemp(dir_path) != NULL);
	CHECK("file_directory_rejected",
		file_util_validate_regular_file(dir_path, "test", "[x] ") != 0);

	unlink(file_path);
	unlink(empty_path);
	rmdir(dir_path);

	return failures;
}
