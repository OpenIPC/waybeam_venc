#include <errno.h>

#define DECLARE_BITRATE_STUB(name) \
    __attribute__((weak)) int name(char index, unsigned int bitrate, unsigned int max_bitrate) \
    { \
        (void)index; \
        (void)bitrate; \
        (void)max_bitrate; \
        return -ENOTSUP; \
    }

/*
 * These weak stubs guarantee that every platform-specific HAL provides a
 * fallback implementation for live bitrate updates. Targets that ship a real
 * handler override the weak definition, while platforms without live bitrate
 * support safely return -ENOTSUP instead of breaking the link.
 */

DECLARE_BITRATE_STUB(i3_video_set_bitrate);
DECLARE_BITRATE_STUB(i6_video_set_bitrate);
DECLARE_BITRATE_STUB(i6c_video_set_bitrate);
DECLARE_BITRATE_STUB(m6_video_set_bitrate);
DECLARE_BITRATE_STUB(rk_video_set_bitrate);
DECLARE_BITRATE_STUB(gm_video_set_bitrate);
DECLARE_BITRATE_STUB(ak_video_set_bitrate);
DECLARE_BITRATE_STUB(v1_video_set_bitrate);
DECLARE_BITRATE_STUB(v2_video_set_bitrate);
DECLARE_BITRATE_STUB(v3_video_set_bitrate);
DECLARE_BITRATE_STUB(v4_video_set_bitrate);
DECLARE_BITRATE_STUB(t31_video_set_bitrate);
DECLARE_BITRATE_STUB(cvi_video_set_bitrate);
