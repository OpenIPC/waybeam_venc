#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>

extern int g_test_pass_count;
extern int g_test_fail_count;

static inline void test_register_result(const char *name, int passed)
{
	if (passed) {
		g_test_pass_count++;
		printf("  PASS  %s\n", name);
	} else {
		g_test_fail_count++;
		printf("  FAIL  %s\n", name);
	}
}

/*
 * Usage:
 *   int failures = 0;
 *   CHECK("test_name", expression);
 */
#define CHECK(name, expr) do { \
	int _ok = !!(expr); \
	test_register_result((name), _ok); \
	if (!_ok) failures++; \
} while (0)

#endif /* TEST_HELPERS_H */
