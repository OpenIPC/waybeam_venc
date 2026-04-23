#ifndef DEBUG_OSD_DRAW_H
#define DEBUG_OSD_DRAW_H

#include <stdint.h>

/* Pure rasterizer for the debug OSD.  Operates on a caller-owned pixel
 * buffer (OsdCanvas) so it can be unit-tested on the host without any
 * MI_RGN dependency.
 *
 * Pixels are 8-bit palette indices (MI_RGN I8 format on the target).
 * The palette itself is owned by src/debug_osd.c and uploaded to the
 * SDK via MI_RGN_Init; the rasterizer never inspects colors. */

typedef struct {
	uint8_t  *pixels;     /* base pointer, one byte per pixel */
	uint32_t  stride_px;  /* pixels per row (bytes for I8) */
	uint32_t  width;
	uint32_t  height;
} OsdCanvas;

typedef struct {
	uint16_t x0, y0, x1, y1;  /* inclusive bbox; empty when x0>x1 or y0>y1 */
} OsdDirty;

/** Reset dirty bbox to empty sentinel (x0=w, y0=h, x1=0, y1=0). */
void osd_dirty_reset(OsdDirty *d, uint32_t w, uint32_t h);

/** Return non-zero if dirty bbox is empty. */
int osd_dirty_empty(const OsdDirty *d);

/** Expand dirty bbox to include (x,y), clamped to canvas bounds. */
void osd_dirty_expand(OsdDirty *d, const OsdCanvas *c, int x, int y);

/** Fill `count` pixels at `row` with palette index `color`. */
void osd_fill_row(uint8_t *row, int count, uint8_t color);

/** Write one pixel; silently clipped when out of bounds. */
void osd_put_pixel(const OsdCanvas *c, int x, int y, uint8_t color);

/** Clear the dirty bbox to 0 (transparent index).  No-op when dirty is empty. */
void osd_clear_dirty(const OsdCanvas *c, const OsdDirty *d);

/** Primitive drawing — updates dirty bbox via `d`. */
void osd_draw_rect(const OsdCanvas *c, OsdDirty *d,
                   uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint8_t color, int filled);
void osd_draw_point(const OsdCanvas *c, OsdDirty *d,
                    uint16_t x, uint16_t y, uint8_t color, int size);
void osd_draw_line(const OsdCanvas *c, OsdDirty *d,
                   uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                   uint8_t color);

/** Glyph rendering — 5x8 bitmap font, scaled by `scale`. */
void osd_draw_char(const OsdCanvas *c, OsdDirty *d,
                   int px, int py, char ch, int scale, uint8_t color);
void osd_draw_string(const OsdCanvas *c, OsdDirty *d,
                     int x, int y, const char *str, int scale,
                     uint8_t color);

/* Palette entry (alpha + RGB).  Layout is a platform-neutral view of the
 * vendor SDK's i6_rgn_pale; src/debug_osd.c copies these into the
 * MI_RGN_Init argument. */
typedef struct {
	uint8_t alpha, red, green, blue;
} OsdPaletteEntry;

#define OSD_PALETTE_SIZE 256

/** Return the fixed palette used by the debug OSD.  Index 0 is always
 *  fully transparent.  Indices 1..8 are the DEBUG_OSD_* colors; 9..255
 *  are zeroed reserved entries. */
const OsdPaletteEntry *osd_palette(void);

#endif /* DEBUG_OSD_DRAW_H */
