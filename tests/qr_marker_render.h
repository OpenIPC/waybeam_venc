#ifndef QR_MARKER_RENDER_H
#define QR_MARKER_RENDER_H

/*
 * The golden Waybeam outer-frame marker, shared by every QR test.
 *
 * Single copy on purpose: this module table IS the fixture.  A second copy
 * would drift silently, and a drifted fixture makes a broken decoder look
 * healthy — the exact failure mode the whole shared-cascade arrangement exists
 * to prevent.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#define FRAME_UNITS 33
#define QR_OFFSET   6
#define QR_SIZE     21

#define QR_EXPECTED_PAYLOAD "P23456789ABCDEFG"

/* Version 1, ECC Q, alphanumeric mode, no quiet-zone modules. */
static const char *const qr_modules[QR_SIZE] = {
	"#######...###.#######",
	"#.....#..##...#.....#",
	"#.###.#.#..#..#.###.#",
	"#.###.#..#..#.#.###.#",
	"#.###.#.##.#..#.###.#",
	"#.....#.###...#.....#",
	"#######.#.#.#.#######",
	"...........##........",
	".#..#.#.#..###.##.#..",
	"######.##.##...##.#..",
	"..######...#.####..#.",
	"#.#..#.####.##...#..#",
	"#...######..##.####.#",
	"........#.#..#.#.####",
	"#######...####.#.###.",
	"#.....#......#..##...",
	"#.###.#.##.##.#.##.#.",
	"#.###.#..#..#..#...##",
	"#.###.#..#.###.##....",
	"#.....#.#....#.#..#.#",
	"#######...#...#.#.###"
};

static int marker_black(double u, double v, int framed)
{
	int module_x;
	int module_y;

	if (u < 0.0 || v < 0.0 ||
	    u >= FRAME_UNITS || v >= FRAME_UNITS)
		return 0;
	if (framed && (u < 2.0 || v < 2.0 ||
	    u >= FRAME_UNITS - 2 || v >= FRAME_UNITS - 2))
		return 1;
	if (u < QR_OFFSET || v < QR_OFFSET ||
	    u >= QR_OFFSET + QR_SIZE || v >= QR_OFFSET + QR_SIZE)
		return 0;

	module_x = (int)floor(u - QR_OFFSET);
	module_y = (int)floor(v - QR_OFFSET);
	return qr_modules[module_y][module_x] == '#';
}

/* Axis-aligned render of a `side`-pixel marker centred in a width×height white
 * frame.  Deliberately simpler than test_qr_marker.c's perspective renderer:
 * the cascade tests care about which STAGE wins, not about projective geometry,
 * which the 768-render corpus already covers. */
static inline void qr_render_square(uint8_t *image, int width, int height,
	int side)
{
	int x0 = (width - side) / 2;
	int y0 = (height - side) / 2;
	int x, y;

	memset(image, 255, (size_t)width * height);
	if (side <= 0 || x0 < 0 || y0 < 0)
		return;
	for (y = 0; y < side; y++) {
		double v = (y + 0.5) * FRAME_UNITS / side;

		for (x = 0; x < side; x++) {
			double u = (x + 0.5) * FRAME_UNITS / side;

			if (marker_black(u, v, 1))
				image[(size_t)(y0 + y) * width + x0 + x] = 0;
		}
	}
}

/* xorshift32 — a deterministic, platform-independent noise source.  rand() is
 * not: its sequence is libc-defined, so a corpus built on it would not be
 * reproducible across the host and the target toolchains. */
static inline uint32_t qr_rng_next(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

#endif /* QR_MARKER_RENDER_H */
