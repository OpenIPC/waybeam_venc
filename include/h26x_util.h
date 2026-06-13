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

#endif /* H26X_UTIL_H */
