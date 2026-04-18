#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "idr_rate_limit.h"
#include "test_helpers.h"

/* Unit test for the per-channel IDR rate limiter.  The gate uses
 * wb_monotonic_us() under the hood, so tests that need to cross the
 * spacing boundary call nanosleep() to advance real monotonic time —
 * we don't mock the clock (it's a trivial vDSO read in production). */

static void sleep_us(uint64_t us)
{
	struct timespec ts;
	ts.tv_sec  = (time_t)(us / 1000000ULL);
	ts.tv_nsec = (long)((us % 1000000ULL) * 1000ULL);
	nanosleep(&ts, NULL);
}

int test_idr_rate_limit(void)
{
	int failures = 0;

	idr_rate_limit_reset();

	/* First call on any channel is always honored. */
	CHECK("idr_first_call_honored", idr_rate_limit_allow(0) == 1);
	CHECK("idr_first_honored_count", idr_rate_limit_honored(0) == 1);
	CHECK("idr_first_dropped_count", idr_rate_limit_dropped(0) == 0);

	/* Back-to-back requests inside the lockout window are coalesced. */
	CHECK("idr_burst2_dropped", idr_rate_limit_allow(0) == 0);
	CHECK("idr_burst3_dropped", idr_rate_limit_allow(0) == 0);
	CHECK("idr_burst4_dropped", idr_rate_limit_allow(0) == 0);
	CHECK("idr_burst_honored_still_1", idr_rate_limit_honored(0) == 1);
	CHECK("idr_burst_dropped_3",       idr_rate_limit_dropped(0) == 3);

	/* Per-channel independence: channel 1 should still get its first. */
	CHECK("idr_chn1_first_honored", idr_rate_limit_allow(1) == 1);
	CHECK("idr_chn1_honored_count", idr_rate_limit_honored(1) == 1);
	CHECK("idr_chn0_unaffected",    idr_rate_limit_honored(0) == 1);

	/* Out-of-range channels bypass the gate (safer fallback). */
	CHECK("idr_bypass_negative", idr_rate_limit_allow(-1) == 1);
	CHECK("idr_bypass_max",      idr_rate_limit_allow(IDR_RATE_LIMIT_MAX_CHANNELS) == 1);
	CHECK("idr_bypass_huge",     idr_rate_limit_allow(9999) == 1);

	/* After the full spacing interval, next request on channel 0 is honored.
	 * 100 ms + 10 ms slack for scheduling noise on the test host. */
	sleep_us(IDR_RATE_LIMIT_MIN_SPACING_US + 10000);
	CHECK("idr_after_spacing_honored", idr_rate_limit_allow(0) == 1);
	CHECK("idr_after_honored_count",   idr_rate_limit_honored(0) == 2);
	CHECK("idr_after_dropped_stable",  idr_rate_limit_dropped(0) == 3);

	/* Reset clears counters. */
	idr_rate_limit_reset();
	CHECK("idr_reset_honored", idr_rate_limit_honored(0) == 0);
	CHECK("idr_reset_dropped", idr_rate_limit_dropped(0) == 0);
	CHECK("idr_reset_first",   idr_rate_limit_allow(0) == 1);

	return failures;
}
