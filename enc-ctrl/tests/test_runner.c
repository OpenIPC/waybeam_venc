#include <stdio.h>
#include "test_helpers.h"

/* Global test counters (defined here, declared extern in test_helpers.h) */
int g_test_pass_count;
int g_test_fail_count;

/* Test suite entry points */
extern int test_ring_buffer(void);
extern int test_scene_estimator(void);
extern int test_gop_controller(void);
extern int test_enc_ctrl(void);

int main(void)
{
	int failures = 0;

	printf("=== enc-ctrl unit tests ===\n\n");

	printf("--- test_ring_buffer ---\n");
	failures += test_ring_buffer();

	printf("\n--- test_scene_estimator ---\n");
	failures += test_scene_estimator();

	printf("\n--- test_gop_controller ---\n");
	failures += test_gop_controller();

	printf("\n--- test_enc_ctrl ---\n");
	failures += test_enc_ctrl();

	printf("\n=== Results: %d passed, %d failed ===\n",
		g_test_pass_count, g_test_fail_count);

	return failures > 0 ? 1 : 0;
}
