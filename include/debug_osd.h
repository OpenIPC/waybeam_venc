#ifndef DEBUG_OSD_H
#define DEBUG_OSD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ARGB4444 color constants: AAAA_RRRR_GGGG_BBBB */
#define DEBUG_OSD_TRANSPARENT     0x0000
#define DEBUG_OSD_WHITE           0xFFFF
#define DEBUG_OSD_RED             0xFF00
#define DEBUG_OSD_GREEN           0xF0F0
#define DEBUG_OSD_BLUE            0xF00F
#define DEBUG_OSD_YELLOW          0xFFF0
#define DEBUG_OSD_CYAN            0xF0FF
#define DEBUG_OSD_SEMITRANS_GREEN 0x40A0
#define DEBUG_OSD_SEMITRANS_BLACK 0xA000

typedef struct DebugOsdState DebugOsdState;

/** Create debug OSD overlay attached to the given VPE port.
 *  Returns NULL if MI_RGN is unavailable or on error.
 *  vpe_port is internally cast to i6_sys_bind *. */
DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                const void *vpe_port);
void debug_osd_destroy(DebugOsdState *osd);

/** Frame lifecycle — bracket all draw calls between begin/end. */
void debug_osd_begin_frame(DebugOsdState *osd);
void debug_osd_end_frame(DebugOsdState *osd);

/** Stats panel text at row position. */
void debug_osd_text(DebugOsdState *osd, int row, const char *label,
                    const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

/** CPU usage sampler — call once per frame, reads /proc/stat at ~2 Hz. */
void debug_osd_sample_cpu(DebugOsdState *osd);
int debug_osd_get_cpu(DebugOsdState *osd);

/** Spatial primitives at real frame coordinates. */
void debug_osd_rect(DebugOsdState *osd, uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, uint16_t color, int filled);
void debug_osd_point(DebugOsdState *osd, uint16_t x, uint16_t y,
                     uint16_t color, int size);
void debug_osd_line(DebugOsdState *osd, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_OSD_H */
