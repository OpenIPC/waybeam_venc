#include "debug_osd_draw.h"
#include "debug_osd.h"
#include "test_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Set to 1 to print current golden hashes on stdout instead of comparing.
 * Run the test once with this flag, paste the printed hashes into the
 * expected string literals below, then set it back to 0. */
#define OSD_UPDATE_GOLDENS 0

/* ── Canvas helpers ──────────────────────────────────────────────────
 * All canvases are allocated with a 16-byte 0xCC guard ring before and
 * after the usable pixel area.  Tests call check_guards() at the end to
 * catch stride miscalculations and OOB writes. */

#define GUARD_BYTES 16
#define GUARD_FILL  0xCC

typedef struct {
	uint8_t *raw;
	size_t raw_bytes;
	OsdCanvas c;
} TestCanvas;

static void tc_alloc(TestCanvas *tc, uint32_t w, uint32_t h)
{
	/* I4 = 2 pixels per byte; round up odd widths to a full byte. */
	uint32_t stride = (w + 1) / 2;
	size_t pixel_bytes = (size_t)stride * h;
	tc->raw_bytes = pixel_bytes + 2 * GUARD_BYTES;
	tc->raw = malloc(tc->raw_bytes);
	memset(tc->raw, GUARD_FILL, tc->raw_bytes);
	tc->c.pixels = tc->raw + GUARD_BYTES;
	tc->c.stride_bytes = stride;
	tc->c.width = w;
	tc->c.height = h;
	memset(tc->c.pixels, 0, pixel_bytes);
}

static void tc_free(TestCanvas *tc)
{
	free(tc->raw);
	tc->raw = NULL;
}

static int tc_guards_intact(const TestCanvas *tc)
{
	for (size_t i = 0; i < GUARD_BYTES; i++) {
		if (tc->raw[i] != GUARD_FILL) return 0;
		if (tc->raw[tc->raw_bytes - 1 - i] != GUARD_FILL) return 0;
	}
	return 1;
}

/* Return the 4-bit palette index at (x, y) by un-packing the nibble.  Uses
 * the rasterizer's own osd_get_pixel so the test reads back the exact same
 * way production does.  Bounds-checked. */
static uint8_t tc_get(const TestCanvas *tc, int x, int y)
{
	return osd_get_pixel(&tc->c, x, y);
}

/* FNV-1a 64-bit hash of the packed nibble buffer — small, reproducible
 * across hosts, no external dependency.  Note: the hash covers physical
 * bytes (not logical pixels), so odd-width canvases include the unused
 * tail nibble, which must be 0 for a clean hash. */
static uint64_t tc_hash(const TestCanvas *tc)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	const uint8_t *p = tc->c.pixels;
	size_t n = (size_t)tc->c.stride_bytes * tc->c.height;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int golden_check(const char *name, uint64_t got, uint64_t expected)
{
#if OSD_UPDATE_GOLDENS
	printf("  GOLDEN  %s = 0x%016llx\n", name, (unsigned long long)got);
	(void)expected;
	return 1;
#else
	if (got == expected) return 1;
	printf("  DIFF    %s: got=0x%016llx expected=0x%016llx\n",
		name, (unsigned long long)got, (unsigned long long)expected);
	return 0;
#endif
}

/* ── Tests ───────────────────────────────────────────────────────────*/

static int test_palette_initialized(void)
{
	int failures = 0;
	const OsdPaletteEntry *pal = osd_palette();

	CHECK("palette_transparent",
		pal[DEBUG_OSD_TRANSPARENT].alpha == 0);

	CHECK("palette_white",
		pal[DEBUG_OSD_WHITE].alpha == 255 &&
		pal[DEBUG_OSD_WHITE].red == 255 &&
		pal[DEBUG_OSD_WHITE].green == 255 &&
		pal[DEBUG_OSD_WHITE].blue == 255);

	CHECK("palette_red",
		pal[DEBUG_OSD_RED].alpha == 255 &&
		pal[DEBUG_OSD_RED].red == 255 &&
		pal[DEBUG_OSD_RED].green == 0 &&
		pal[DEBUG_OSD_RED].blue == 0);

	CHECK("palette_yellow",
		pal[DEBUG_OSD_YELLOW].alpha == 255 &&
		pal[DEBUG_OSD_YELLOW].red == 255 &&
		pal[DEBUG_OSD_YELLOW].green == 255 &&
		pal[DEBUG_OSD_YELLOW].blue == 0);

	/* Alpha values of semi-transparent entries must match the 4-bit
	 * ARGB4444 codes the legacy code used (0x4 → 68, 0xA → 170). */
	CHECK("palette_semitrans_green",
		pal[DEBUG_OSD_SEMITRANS_GREEN].alpha == 68 &&
		pal[DEBUG_OSD_SEMITRANS_GREEN].red == 0 &&
		pal[DEBUG_OSD_SEMITRANS_GREEN].green == 170 &&
		pal[DEBUG_OSD_SEMITRANS_GREEN].blue == 0);

	CHECK("palette_semitrans_black",
		pal[DEBUG_OSD_SEMITRANS_BLACK].alpha == 170 &&
		pal[DEBUG_OSD_SEMITRANS_BLACK].red == 0 &&
		pal[DEBUG_OSD_SEMITRANS_BLACK].green == 0 &&
		pal[DEBUG_OSD_SEMITRANS_BLACK].blue == 0);

	/* Reserved entries must be zeroed. */
	int reserved_clean = 1;
	for (int i = 9; i < OSD_PALETTE_SIZE; i++) {
		if (pal[i].alpha || pal[i].red || pal[i].green || pal[i].blue) {
			reserved_clean = 0;
			break;
		}
	}
	CHECK("palette_reserved_zero", reserved_clean);

	return failures;
}

static int test_fill_pixels_bounds(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 32, 4);

	/* Fill 10 pixels in the middle of row 1, starting at odd x=5
	 * (exercises unaligned-start nibble + middle bytes + unaligned-end). */
	osd_fill_pixels(&tc.c, 5, 1, 10, 0x0B);

	int ok = 1;
	for (int x = 5; x < 15; x++) ok &= (tc_get(&tc, x, 1) == 0x0B);
	CHECK("fill_paints_range", ok);
	CHECK("fill_leaves_left_untouched", tc_get(&tc, 4, 1) == 0x00);
	CHECK("fill_leaves_right_untouched", tc_get(&tc, 15, 1) == 0x00);
	CHECK("fill_leaves_other_rows", tc_get(&tc, 10, 0) == 0x00
		&& tc_get(&tc, 10, 2) == 0x00);
	CHECK("fill_guards_intact", tc_guards_intact(&tc));

	/* Edge cases: count=0, count=1, even-aligned start */
	tc_free(&tc);
	tc_alloc(&tc, 32, 2);
	osd_fill_pixels(&tc.c, 0, 0, 0, 0x0E);
	CHECK("fill_count_zero_is_noop", tc_get(&tc, 0, 0) == 0x00);
	osd_fill_pixels(&tc.c, 3, 0, 1, 0x0E);
	CHECK("fill_count_one_odd_x", tc_get(&tc, 3, 0) == 0x0E
		&& tc_get(&tc, 2, 0) == 0x00 && tc_get(&tc, 4, 0) == 0x00);
	osd_fill_pixels(&tc.c, 6, 0, 1, 0x0E);
	CHECK("fill_count_one_even_x", tc_get(&tc, 6, 0) == 0x0E
		&& tc_get(&tc, 5, 0) == 0x00 && tc_get(&tc, 7, 0) == 0x00);

	/* Even-aligned start + even count exercises the pure-memset path */
	tc_free(&tc);
	tc_alloc(&tc, 32, 2);
	osd_fill_pixels(&tc.c, 4, 0, 8, 0x0A);
	for (int x = 4; x < 12; x++)
		CHECK("fill_aligned_byte_path", tc_get(&tc, x, 0) == 0x0A);
	CHECK("fill_aligned_left_untouched", tc_get(&tc, 3, 0) == 0x00);
	CHECK("fill_aligned_right_untouched", tc_get(&tc, 12, 0) == 0x00);

	/* Negative-x clipping: shrink count, anchor at x=0 */
	tc_free(&tc);
	tc_alloc(&tc, 16, 2);
	osd_fill_pixels(&tc.c, -3, 0, 8, 0x09);
	for (int x = 0; x < 5; x++)
		CHECK("fill_neg_x_clipped", tc_get(&tc, x, 0) == 0x09);
	CHECK("fill_neg_x_no_overflow", tc_get(&tc, 5, 0) == 0x00);

	/* Past-right clipping */
	osd_fill_pixels(&tc.c, 14, 1, 8, 0x07);
	CHECK("fill_past_right_clipped",
		tc_get(&tc, 14, 1) == 0x07 && tc_get(&tc, 15, 1) == 0x07);
	CHECK("fill_past_right_guards", tc_guards_intact(&tc));

	tc_free(&tc);
	return failures;
}

static int test_put_pixel_clipping(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 8, 4);

	/* I4 nibble packing — verify that writes to even x and odd x both
	 * land in the right pixel without corrupting the sibling nibble. */
	osd_put_pixel(&tc.c, 3, 2, 0x02);  /* odd x → high nibble */
	CHECK("put_pixel_odd_x", tc_get(&tc, 3, 2) == 0x02);
	CHECK("put_pixel_odd_x_sibling_clean", tc_get(&tc, 2, 2) == 0x00);
	osd_put_pixel(&tc.c, 4, 2, 0x05);  /* even x → low nibble */
	CHECK("put_pixel_even_x", tc_get(&tc, 4, 2) == 0x05);
	CHECK("put_pixel_even_x_sibling_clean", tc_get(&tc, 5, 2) == 0x00);

	osd_put_pixel(&tc.c, -1, 0, 0x0D);
	osd_put_pixel(&tc.c, 0, -1, 0x0D);
	osd_put_pixel(&tc.c, 8, 0, 0x0D);
	osd_put_pixel(&tc.c, 0, 4, 0x0D);
	osd_put_pixel(&tc.c, 100, 100, 0x0D);
	CHECK("put_pixel_oob_no_op_guards", tc_guards_intact(&tc));
	int clean = 1;
	for (uint32_t y = 0; y < tc.c.height; y++)
		for (uint32_t x = 0; x < tc.c.width; x++) {
			int set = ((x == 3 && y == 2) || (x == 4 && y == 2));
			if (!set && tc_get(&tc, x, y) != 0)
				clean = 0;
		}
	CHECK("put_pixel_oob_leaves_canvas_clean", clean);

	tc_free(&tc);
	return failures;
}

static int test_rect_filled(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 16, 8);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	osd_draw_rect(&tc.c, &d, 4, 2, 5, 3, 0x0F, 1);

	int inside_ok = 1, outside_ok = 1;
	for (uint32_t y = 0; y < tc.c.height; y++) {
		for (uint32_t x = 0; x < tc.c.width; x++) {
			int in = (x >= 4 && x < 9 && y >= 2 && y < 5);
			uint8_t v = tc_get(&tc, x, y);
			if (in && v != 0x0F) inside_ok = 0;
			if (!in && v != 0x00) outside_ok = 0;
		}
	}
	CHECK("rect_filled_interior", inside_ok);
	CHECK("rect_filled_outside_untouched", outside_ok);
	CHECK("rect_filled_dirty_x0y0", d.x0 == 4 && d.y0 == 2);
	CHECK("rect_filled_dirty_x1y1", d.x1 == 8 && d.y1 == 4);
	CHECK("rect_filled_guards_intact", tc_guards_intact(&tc));

	tc_free(&tc);
	return failures;
}

static int test_rect_hollow(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 16, 10);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	/* 6x5 hollow rect at (3,2) → covers x=3..8, y=2..6 */
	osd_draw_rect(&tc.c, &d, 3, 2, 6, 5, 0x05, 0);

	int border_ok = 1, interior_clean = 1;
	for (uint32_t y = 0; y < tc.c.height; y++) {
		for (uint32_t x = 0; x < tc.c.width; x++) {
			int in_bbox = (x >= 3 && x <= 8 && y >= 2 && y <= 6);
			int on_border = in_bbox && (x == 3 || x == 8
				|| y == 2 || y == 6);
			uint8_t v = tc_get(&tc, x, y);
			if (on_border && v != 0x05) border_ok = 0;
			if (in_bbox && !on_border && v != 0x00)
				interior_clean = 0;
		}
	}
	CHECK("rect_hollow_border", border_ok);
	CHECK("rect_hollow_interior_clean", interior_clean);
	CHECK("rect_hollow_guards_intact", tc_guards_intact(&tc));

	tc_free(&tc);
	return failures;
}

static int test_rect_clipped(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 8, 8);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	osd_draw_rect(&tc.c, &d, 5, 5, 10, 10, 0x01, 1);

	int drawn_ok = 1;
	for (uint32_t y = 5; y < tc.c.height; y++)
		for (uint32_t x = 5; x < tc.c.width; x++)
			if (tc_get(&tc, x, y) != 0x01) drawn_ok = 0;
	CHECK("rect_clipped_drawn_portion", drawn_ok);
	CHECK("rect_clipped_no_oob", tc_guards_intact(&tc));

	/* Fully offscreen rect — no-op */
	TestCanvas tc2;
	tc_alloc(&tc2, 8, 8);
	OsdDirty d2;
	osd_dirty_reset(&d2, tc2.c.width, tc2.c.height);
	osd_draw_rect(&tc2.c, &d2, 100, 100, 5, 5, 0x01, 1);
	int clean = 1;
	for (uint32_t y = 0; y < tc2.c.height; y++)
		for (uint32_t x = 0; x < tc2.c.width; x++)
			if (tc_get(&tc2, x, y) != 0) clean = 0;
	CHECK("rect_offscreen_noop", clean);
	CHECK("rect_offscreen_dirty_empty", osd_dirty_empty(&d2));

	tc_free(&tc);
	tc_free(&tc2);
	return failures;
}

static int test_line_bresenham(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 8, 8);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	/* 45° diagonal from (0,0) to (5,5) */
	osd_draw_line(&tc.c, &d, 0, 0, 5, 5, 0x0B);

	int diag_ok = 1;
	for (int i = 0; i <= 5; i++)
		if (tc_get(&tc, i, i) != 0x0B) diag_ok = 0;
	CHECK("line_diagonal_drawn", diag_ok);

	TestCanvas tc2;
	tc_alloc(&tc2, 8, 8);
	OsdDirty d2;
	osd_dirty_reset(&d2, tc2.c.width, tc2.c.height);
	osd_draw_line(&tc2.c, &d2, 5, 5, 0, 0, 0x0B);
	int same = 1;
	for (int i = 0; i <= 5; i++)
		if (tc_get(&tc2, i, i) != 0x0B) same = 0;
	CHECK("line_reverse_same_pixels", same);

	CHECK("line_guards_intact", tc_guards_intact(&tc)
		&& tc_guards_intact(&tc2));
	CHECK("line_dirty_bbox", d.x0 == 0 && d.y0 == 0
		&& d.x1 == 5 && d.y1 == 5);

	tc_free(&tc);
	tc_free(&tc2);
	return failures;
}

static int test_point_cross(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 10, 10);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	osd_draw_point(&tc.c, &d, 5, 5, 0x01, 2);

	int horizontal_ok = 1, vertical_ok = 1;
	for (int dx = -2; dx <= 2; dx++)
		if (tc_get(&tc, 5 + dx, 5) != 0x01) horizontal_ok = 0;
	for (int dy = -2; dy <= 2; dy++)
		if (tc_get(&tc, 5, 5 + dy) != 0x01) vertical_ok = 0;
	CHECK("point_horizontal_arm", horizontal_ok);
	CHECK("point_vertical_arm", vertical_ok);

	CHECK("point_corner_empty", tc_get(&tc, 3, 3) == 0
		&& tc_get(&tc, 7, 7) == 0);

	tc_free(&tc);
	return failures;
}

static int test_draw_char_A_scale1(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 8, 10);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	osd_draw_char(&tc.c, &d, 1, 1, 'A', 1, 0x01);

	/* Expected bitmap for 'A' (5x8, bit 4 = leftmost):
	 *   .###.     0x0E
	 *   #...#     0x11
	 *   #...#     0x11
	 *   #####     0x1F
	 *   #...#     0x11
	 *   #...#     0x11
	 *   #...#     0x11
	 *   .....     0x00
	 */
	static const uint8_t expected[8] = {
		0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00
	};
	int ok = 1;
	for (int gy = 0; gy < 8; gy++) {
		uint8_t bits = expected[gy];
		for (int gx = 0; gx < 5; gx++) {
			int expected_set = !!(bits & (0x10 >> gx));
			uint8_t v = tc_get(&tc, 1 + gx, 1 + gy);
			int set = (v == 0x01);
			if (expected_set != set) ok = 0;
		}
	}
	CHECK("draw_char_A_bitmap", ok);
	CHECK("draw_char_guards_intact", tc_guards_intact(&tc));

	tc_free(&tc);
	return failures;
}

static int test_draw_char_scaled(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 32, 32);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	osd_draw_char(&tc.c, &d, 0, 0, '0', 3, 0x01);

	int blocks_ok = 1;
	for (int sy = 0; sy < 3; sy++)
		for (int sx = 0; sx < 3; sx++)
			if (tc_get(&tc, 3 + sx, 0 + sy) != 0x01)
				blocks_ok = 0;
	for (int sy = 0; sy < 3; sy++)
		for (int sx = 0; sx < 3; sx++)
			if (tc_get(&tc, sx, sy) != 0) blocks_ok = 0;
	CHECK("draw_char_scale3_blocks", blocks_ok);
	CHECK("draw_char_scale3_dirty_bbox",
		d.x0 == 0 && d.y0 == 0 && d.x1 == 14 && d.y1 == 23);

	tc_free(&tc);
	return failures;
}

static int test_draw_string_golden(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 64, 32);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	osd_draw_string(&tc.c, &d, 2, 2, "fps: 30", 3, DEBUG_OSD_WHITE);

	uint64_t h = tc_hash(&tc);
	CHECK("draw_string_golden", golden_check(
		"draw_string_fps_30", h, 0x84663957bc2d8246ULL));
	CHECK("draw_string_guards_intact", tc_guards_intact(&tc));

	tc_free(&tc);
	return failures;
}

static int test_composite_scene_golden(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 128, 64);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	/* Two text panels + 3 nested EIS rects — covers every primitive in
	 * one scene.  Reproduces roughly what star6e_runtime.c draws. */
	osd_draw_rect(&tc.c, &d, 6, 7, 60, 26, DEBUG_OSD_SEMITRANS_BLACK, 1);
	osd_draw_string(&tc.c, &d, 8, 8, "fps: 30", 3, DEBUG_OSD_WHITE);
	osd_draw_rect(&tc.c, &d, 6, 33, 60, 26, DEBUG_OSD_SEMITRANS_BLACK, 1);
	osd_draw_string(&tc.c, &d, 8, 34, "cpu: 42", 3, DEBUG_OSD_WHITE);
	osd_draw_rect(&tc.c, &d, 80,  4, 40, 40, DEBUG_OSD_WHITE, 0);
	osd_draw_rect(&tc.c, &d, 85,  9, 30, 30, DEBUG_OSD_YELLOW, 0);
	osd_draw_rect(&tc.c, &d, 90, 14, 20, 20,
		DEBUG_OSD_SEMITRANS_GREEN, 1);

	uint64_t h = tc_hash(&tc);
	CHECK("composite_scene_golden", golden_check(
		"composite_scene", h, 0xe3d17e4522b7ae84ULL));
	CHECK("composite_scene_guards_intact", tc_guards_intact(&tc));

	tc_free(&tc);
	return failures;
}

static int test_dirty_rect_expand(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 200, 200);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	CHECK("dirty_starts_empty", osd_dirty_empty(&d));

	osd_draw_rect(&tc.c, &d, 10, 10, 20, 20, 0x01, 1);
	CHECK("dirty_after_rect",
		d.x0 == 10 && d.y0 == 10 && d.x1 == 29 && d.y1 == 29);

	osd_draw_char(&tc.c, &d, 100, 100, 'A', 3, 0x01);
	/* 'A' at scale=3 → 15x24 bbox starting at 100,100 → (100,100)-(114,123) */
	CHECK("dirty_after_char",
		d.x0 == 10 && d.y0 == 10 && d.x1 == 114 && d.y1 == 123);

	osd_dirty_expand(&d, &tc.c, -5, 500);
	CHECK("dirty_expand_clamped",
		d.x0 == 0 && d.y1 == 199);

	tc_free(&tc);
	return failures;
}

static int test_dirty_rect_clear(void)
{
	int failures = 0;
	TestCanvas tc;
	tc_alloc(&tc, 32, 16);
	OsdDirty d;
	osd_dirty_reset(&d, tc.c.width, tc.c.height);

	osd_draw_rect(&tc.c, &d, 2, 2, 4, 4, 0x01, 1);
	osd_draw_rect(&tc.c, &d, 20, 8, 4, 4, 0x01, 1);

	/* Paint a pixel outside the dirty bbox — should survive the clear.
	 * Use osd_put_pixel so the I4 nibble packing is correct. */
	osd_put_pixel(&tc.c, 31, 0, 0x0A);

	osd_clear_dirty(&tc.c, &d);

	int zeroed = 1;
	for (uint32_t y = d.y0; y <= d.y1; y++)
		for (uint32_t x = d.x0; x <= d.x1; x++)
			if (tc_get(&tc, x, y) != 0) zeroed = 0;
	CHECK("clear_zeroes_dirty", zeroed);
	CHECK("clear_preserves_outside",
		tc_get(&tc, 31, 0) == 0x0A);
	CHECK("clear_guards_intact", tc_guards_intact(&tc));

	tc_free(&tc);
	return failures;
}

int test_debug_osd(void)
{
	int failures = 0;
	failures += test_palette_initialized();
	failures += test_fill_pixels_bounds();
	failures += test_put_pixel_clipping();
	failures += test_rect_filled();
	failures += test_rect_hollow();
	failures += test_rect_clipped();
	failures += test_line_bresenham();
	failures += test_point_cross();
	failures += test_draw_char_A_scale1();
	failures += test_draw_char_scaled();
	failures += test_draw_string_golden();
	failures += test_composite_scene_golden();
	failures += test_dirty_rect_expand();
	failures += test_dirty_rect_clear();
	return failures;
}
