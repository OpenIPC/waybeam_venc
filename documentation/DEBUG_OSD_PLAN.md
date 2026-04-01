# Debug OSD

Debug-only on-screen overlay for waybeam_venc. Renders directly into the
video stream via SigmaStar MI_RGN canvas on VPE channel 0. Disabled by
default, zero runtime cost when off.

## Enabling

Config (`/etc/venc.json`):
```json
"debug": { "showOsd": true }
```

HTTP API (requires pipeline restart):
```
GET /api/v1/set?debug.show_osd=true
```

**Prerequisite**: waybeam-hub's `mod_osd_render` must be stopped. Both use
`MI_RGN_Init` (global SDK state) on the same VPE port. Only one process
can own MI_RGN at a time.

## Architecture

Single full-frame ARGB4444 canvas (1920×1080 × 2 bytes = ~4 MB, memory-mapped).
Per-frame lifecycle:

1. `debug_osd_begin_frame()` — re-acquire canvas (SDK double-buffers), clear
   previous frame's dirty region only
2. Draw calls — text, rects, points, lines write directly into canvas pixels
3. `debug_osd_end_frame()` — `MI_RGN_UpdateCanvas` commits (buffer swap)

Dirty-rect tracking ensures only the drawn area is cleared each frame,
keeping bandwidth minimal even with a full-frame canvas allocation.

Row fills use ARM NEON intrinsics (`vst1q_u16`, 8 pixels per store).

## API Reference

All functions are no-ops when `osd` is NULL. The `DebugOsdState *` pointer
is available from `Star6ePipelineState.debug_osd` — NULL when debug OSD is
disabled or MI_RGN is unavailable.

### Lifecycle

```c
#include "debug_osd.h"

/* Create — called by star6e_pipeline.c during pipeline init.
 * Returns NULL if MI_RGN dlopen fails or showOsd is false. */
DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                const void *vpe_port);

/* Destroy — called during pipeline stop. */
void debug_osd_destroy(DebugOsdState *osd);

/* Frame bracket — all draw calls go between begin and end. */
void debug_osd_begin_frame(DebugOsdState *osd);  /* clears previous dirty area */
void debug_osd_end_frame(DebugOsdState *osd);    /* commits canvas to display */
```

### Text

```c
/* Draw "label: value" at row position in the stats panel (top-left corner).
 * Row 0 is topmost. Font is 5px wide × 8px tall, scaled by font_scale (3x).
 * Semi-transparent black background drawn automatically behind text. */
void debug_osd_text(DebugOsdState *osd, int row, const char *label,
                    const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
```

### Spatial Primitives

All coordinates are real frame pixels (0,0 = top-left).

```c
/* Rectangle — outline (filled=0) or solid fill (filled=1).
 * Filled rects use NEON-accelerated row fill.
 * Outline rects draw 4 edges (1px thick at frame scale). */
void debug_osd_rect(DebugOsdState *osd, uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, uint16_t color, int filled);

/* Cross-shaped marker at (x,y), extending ±size pixels. */
void debug_osd_point(DebugOsdState *osd, uint16_t x, uint16_t y,
                     uint16_t color, int size);

/* Line from (x0,y0) to (x1,y1) via Bresenham's algorithm. */
void debug_osd_line(DebugOsdState *osd, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1, uint16_t color);
```

### CPU Sampling

```c
/* Sample /proc/stat at ~2 Hz. Call once per frame. */
void debug_osd_sample_cpu(DebugOsdState *osd);

/* Return last sampled CPU% (0-100). */
int debug_osd_get_cpu(DebugOsdState *osd);
```

### Colors (ARGB4444)

16-bit pixel format: `AAAA_RRRR_GGGG_BBBB`. Defined in `debug_osd.h`:

| Constant | Value | Description |
|----------|-------|-------------|
| `DEBUG_OSD_TRANSPARENT` | `0x0000` | Fully transparent |
| `DEBUG_OSD_WHITE` | `0xFFFF` | Opaque white |
| `DEBUG_OSD_RED` | `0xFF00` | Opaque red |
| `DEBUG_OSD_GREEN` | `0xF0F0` | Opaque green |
| `DEBUG_OSD_BLUE` | `0xF00F` | Opaque blue |
| `DEBUG_OSD_YELLOW` | `0xFFF0` | Opaque yellow |
| `DEBUG_OSD_CYAN` | `0xF0FF` | Opaque cyan |
| `DEBUG_OSD_SEMITRANS_GREEN` | `0x40A0` | Semi-transparent green |
| `DEBUG_OSD_SEMITRANS_BLACK` | `0xA000` | Semi-transparent black |

Custom colors: compose as `(alpha << 12) | (red << 8) | (green << 4) | blue`,
each nibble 0-F.

## Adding Debug Output from a New Module

Any module with access to `Star6ePipelineState *ps` can draw debug
visualizations. The pattern:

```c
#include "debug_osd.h"

/* In your per-frame update function: */
void my_module_update(Star6ePipelineState *ps, ...)
{
    /* Guard — no-op if debug OSD disabled */
    if (!ps->debug_osd) return;

    /* Text stats — pick row numbers that don't conflict with existing rows.
     * Rows 0-1 are fps/cpu, rows 3-5 are EIS (when active). */
    debug_osd_text(ps->debug_osd, 7, "my_val", "%d", some_value);
    debug_osd_text(ps->debug_osd, 8, "my_str", "%s", some_string);

    /* Spatial primitives at real frame coordinates */
    debug_osd_rect(ps->debug_osd, roi_x, roi_y, roi_w, roi_h,
                   DEBUG_OSD_CYAN, 0);  /* outline */
    debug_osd_point(ps->debug_osd, center_x, center_y,
                    DEBUG_OSD_RED, 4);   /* cross marker, ±4 px */
    debug_osd_line(ps->debug_osd, x0, y0, x1, y1,
                   DEBUG_OSD_YELLOW);    /* vector */
}
```

**Integration in the frame loop** (`star6e_runtime.c`): add your call
between `debug_osd_begin_frame` and `debug_osd_end_frame`:

```c
if (ps->debug_osd) {
    debug_osd_begin_frame(ps->debug_osd);
    debug_osd_sample_cpu(ps->debug_osd);

    /* Built-in stats */
    debug_osd_text(ps->debug_osd, 0, "fps", "%u", osd_fps);
    debug_osd_text(ps->debug_osd, 1, "cpu", "%d%%", ...);

    /* EIS visualization (if active) */
    if (ps->eis) { ... }

    /* YOUR MODULE — add here */
    my_module_update(ps, ...);

    debug_osd_end_frame(ps->debug_osd);
}
```

**Rules:**
- All draw calls must be between `begin_frame` / `end_frame`
- The frame loop is single-threaded — no locking needed
- Use rows 6+ for text to avoid conflicting with built-in stats (0-1) and
  EIS (3-5)
- Coordinates are frame pixels — scale if your data uses different units
- Keep draw operations minimal — each filled rect and text line has a cost

## Implementation Details

- **dlopen**: `libmi_rgn.so` loaded at runtime, 8 symbols resolved. If the
  library is absent, `debug_osd_create` returns NULL and all consumers no-op.
- **RGN module ID**: MI_RGN uses its own enum (`E_MI_RGN_MODID_VPE = 0`),
  not the system module enum (`I6_SYS_MOD_VPE = 11`). The ChnPort is built
  manually, not copied from the pipeline's `vpe_port`.
- **Double buffering**: `MI_RGN_GetCanvasInfo` must be called every frame
  because `MI_RGN_UpdateCanvas` swaps buffers and the `virtAddr` changes.
- **Font**: 8×8 bitmap, 5px effective width, 95 printable ASCII glyphs
  (760 bytes). Bit 4 = leftmost pixel. Scaled 3× by default.
- **NEON fill**: `fill_row` uses `vst1q_u16` (8 pixels per store) with
  alignment prologue and scalar tail. Word-fill fallback for non-NEON builds.
- **Star6E only**: `src/debug_osd.c` is in `STAR6E_ONLY_SRC`. Non-Star6E
  builds get stub functions that return NULL/void.
