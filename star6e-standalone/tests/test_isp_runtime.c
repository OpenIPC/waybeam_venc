#include "isp_runtime.h"

#include "test_helpers.h"

static int test_isp_runtime_null_safe(void)
{
	int failures = 0;

	isp_runtime_lib_reset(NULL);
	CHECK("isp runtime null path ok",
		isp_runtime_load_bin_file(NULL, NULL) == 0);
	CHECK("isp runtime empty path ok",
		isp_runtime_load_bin_file("", NULL) == 0);
	return failures;
}

static int test_isp_runtime_missing_file(void)
{
	int failures = 0;

	CHECK("isp runtime missing file fails",
		isp_runtime_load_bin_file("/tmp/isp_runtime_missing_file.bin",
			NULL) != 0);
	return failures;
}

int test_isp_runtime(void)
{
	int failures = 0;

	failures += test_isp_runtime_null_safe();
	failures += test_isp_runtime_missing_file();
	return failures;
}
