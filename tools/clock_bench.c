/*
 * clock_bench — ns/call microbenchmark for clock_gettime variants.
 *
 * Cross-compile with the star6e toolchain and run on the vehicle to
 * validate the PR-A assumption that CLOCK_MONOTONIC (vDSO fast path on
 * ARMv7) is much cheaper than CLOCK_MONOTONIC_RAW (real syscall).
 *
 * Build:
 *   toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-gcc \
 *       -O2 -o rtp_timing_probe_clock tools/clock_bench.c
 *
 * Run:
 *   scp -O rtp_timing_probe_clock root@192.168.1.13:/tmp/
 *   ssh root@192.168.1.13 /tmp/rtp_timing_probe_clock
 *
 * Output (one line per clock, ns/call averaged over 1M iterations):
 *   MONOTONIC_RAW:  1512.3 ns/call
 *   MONOTONIC:        98.7 ns/call
 *   MONOTONIC_COARSE:  12.4 ns/call
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define ITERS (1000 * 1000)

static double bench_clock(clockid_t clk, const char *name)
{
	struct timespec start, end, scratch;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < ITERS; i++)
		clock_gettime(clk, &scratch);
	clock_gettime(CLOCK_MONOTONIC, &end);
	double ns = (end.tv_sec - start.tv_sec) * 1e9 +
	            (end.tv_nsec - start.tv_nsec);
	double ns_per_call = ns / ITERS;
	printf("%-20s %8.1f ns/call\n", name, ns_per_call);
	return ns_per_call;
}

int main(void)
{
	(void)bench_clock(CLOCK_MONOTONIC_RAW, "MONOTONIC_RAW:");
	(void)bench_clock(CLOCK_MONOTONIC, "MONOTONIC:");
	(void)bench_clock(CLOCK_MONOTONIC_COARSE, "MONOTONIC_COARSE:");
	return 0;
}
