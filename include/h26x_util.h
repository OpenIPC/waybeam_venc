#ifndef H26X_UTIL_H
#define H26X_UTIL_H

#include <stddef.h>
#include <stdint.h>

/** Remove Annex-B start codes from the front of a NAL buffer. */
void h26x_util_strip_start_code(const uint8_t **data, size_t *length);

/** Return the H.264 NAL unit type, or 0 for invalid input. */
uint8_t h26x_util_h264_nalu_type(const uint8_t *data, size_t len);

/** Return the HEVC NAL unit type, or 0 for invalid input. */
uint8_t h26x_util_hevc_nalu_type(const uint8_t *data, size_t len);

/** Iterate NAL payloads in an Annex-B access unit.
 *
 * Set *cursor=0 before the first call. On success returns 1, advances cursor,
 * and returns a NAL without its 3/4-byte start code or trailing_zero_8bits.
 * Returns 0 at end of input or for invalid arguments. */
int h26x_util_annexb_next(const uint8_t *data, size_t len, size_t *cursor,
	const uint8_t **nal, size_t *nal_len);

/** Rewrite every single-layer HEVC TRAIL_R NAL (type 1) in an Annex-B access
 * unit to TRAIL_N (type 0). Returns the number of NAL headers changed. This
 * propagates an encoder's non-reference frame designation to generic decoders;
 * callers must only use it when vendor frame metadata says the whole picture
 * is not used for reference. */
size_t h26x_util_hevc_patch_trail_r_to_n(uint8_t *data, size_t len);

#endif /* H26X_UTIL_H */
