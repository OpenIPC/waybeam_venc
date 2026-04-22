#include "idr_rate_limit.h"

#include "timing.h"

#include <string.h>

struct chn_state {
	uint64_t last_us;
	uint32_t honored;
	uint32_t dropped;
};

static struct chn_state g_state[IDR_RATE_LIMIT_MAX_CHANNELS];

int idr_rate_limit_allow(int venc_chn)
{
	struct chn_state *s;
	uint64_t now;
	uint64_t last;

	if (venc_chn < 0 || venc_chn >= IDR_RATE_LIMIT_MAX_CHANNELS)
		return 1;

	s = &g_state[venc_chn];
	now = wb_monotonic_us();

	/* CAS loop: only one caller wins each spacing window.  A naive
	 * load-then-store leaves a race where two threads both pass the
	 * spacing check on the same `last` value and both honor an IDR
	 * inside the window — defeating storm coalescing.  ACQ_REL on the
	 * winning store synchronizes with the next caller's load. */
	last = __atomic_load_n(&s->last_us, __ATOMIC_ACQUIRE);
	for (;;) {
		if (last != 0 && (now - last) < IDR_RATE_LIMIT_MIN_SPACING_US) {
			__atomic_add_fetch(&s->dropped, 1, __ATOMIC_RELAXED);
			return 0;
		}
		if (__atomic_compare_exchange_n(&s->last_us, &last, now,
		    /* weak */ 0,
		    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			break;
		/* Lost the race — `last` was updated by the CAS to the
		 * current value of last_us; retry the spacing check. */
	}

	__atomic_add_fetch(&s->honored, 1, __ATOMIC_RELAXED);
	return 1;
}

uint32_t idr_rate_limit_honored(int venc_chn)
{
	if (venc_chn < 0 || venc_chn >= IDR_RATE_LIMIT_MAX_CHANNELS)
		return 0;
	return __atomic_load_n(&g_state[venc_chn].honored, __ATOMIC_RELAXED);
}

uint32_t idr_rate_limit_dropped(int venc_chn)
{
	if (venc_chn < 0 || venc_chn >= IDR_RATE_LIMIT_MAX_CHANNELS)
		return 0;
	return __atomic_load_n(&g_state[venc_chn].dropped, __ATOMIC_RELAXED);
}

void idr_rate_limit_reset(void)
{
	memset(g_state, 0, sizeof(g_state));
}
