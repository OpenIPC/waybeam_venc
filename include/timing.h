/*
 * Monotonic-microsecond clock helper.
 *
 * Uses CLOCK_MONOTONIC (not CLOCK_MONOTONIC_RAW) so ARMv7 reads go through
 * the vDSO fast path (~100 ns) instead of a real syscall (~1500 ns). NTP
 * slew on a running system is <500 ppm, which translates to <4 us drift
 * over a 60 s measurement window — well inside acceptable error for local
 * frame-timing metrics.
 */

#ifndef WAYBEAM_TIMING_H
#define WAYBEAM_TIMING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t wb_monotonic_us(void);

#ifdef __cplusplus
}
#endif

#endif /* WAYBEAM_TIMING_H */
