/*
 * Per-VENC-channel IDR rate limiter.
 *
 * IDR (keyframe) requests originate from several unrelated producers:
 * scene detector, HTTP API (/api/v1/idr, /api/v1/dual/idr), recorder
 * start, controls-apply paths.  Without coordination a bug-driven burst
 * (for example a mis-tuned scene threshold during a camera pan) can
 * crater per-frame bitrate by back-to-back forced keyframes.  This
 * module enforces a minimum spacing between honored IDRs per channel;
 * requests inside the lockout window are silently coalesced and counted
 * in a rate-limited counter.
 *
 * Policy is compile-time:
 *   IDR_RATE_LIMIT_MIN_SPACING_US = 100000 (100 ms)
 *
 * 100 ms at 120 fps = 12 frames between honored IDRs, well above the
 * sensible scene-cooldown floor and well below the typical GOP period
 * (≈83 ms at GOP=10) — so auto-GOP IDRs are unaffected (they're not
 * RequestIdr-driven), forced IDRs coalesce when fired in a storm.
 *
 * Thread-safety: each channel's state is a pair of __atomic_ fields;
 * multiple callers may race without locking — last writer wins on
 * `last_us`, counter is atomic-increment.  No mutex required.
 */

#ifndef IDR_RATE_LIMIT_H
#define IDR_RATE_LIMIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDR_RATE_LIMIT_MAX_CHANNELS   8
#define IDR_RATE_LIMIT_MIN_SPACING_US 100000u

/* Returns 1 if the request should be honored (now > last + min_spacing),
 * 0 if it is rate-limited (counter incremented).  Always returns 1 on
 * the very first call per channel.  Out-of-range channel indices are
 * always honored (bypass — safer than silently dropping). */
int idr_rate_limit_allow(int venc_chn);

/* Read the honored count for this channel since process start. */
uint32_t idr_rate_limit_honored(int venc_chn);

/* Read the rate-limited count for this channel since process start. */
uint32_t idr_rate_limit_dropped(int venc_chn);

/* Test-only helper: reset all per-channel state.  Call from unit tests
 * between cases; no effect on production init path (state is zeroed by
 * BSS already). */
void idr_rate_limit_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* IDR_RATE_LIMIT_H */
