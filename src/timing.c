#include "timing.h"

#include <time.h>

uint64_t wb_monotonic_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)(ts.tv_nsec / 1000);
}
