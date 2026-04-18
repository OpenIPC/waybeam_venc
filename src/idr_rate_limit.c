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
	if (venc_chn < 0 || venc_chn >= IDR_RATE_LIMIT_MAX_CHANNELS)
		return 1;

	struct chn_state *s = &g_state[venc_chn];
	uint64_t now = wb_monotonic_us();
	uint64_t last = __atomic_load_n(&s->last_us, __ATOMIC_RELAXED);

	if (last != 0 && (now - last) < IDR_RATE_LIMIT_MIN_SPACING_US) {
		__atomic_add_fetch(&s->dropped, 1, __ATOMIC_RELAXED);
		return 0;
	}

	__atomic_store_n(&s->last_us, now, __ATOMIC_RELAXED);
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
