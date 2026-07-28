/* qr_decode.c — decode a QR code from a binary P5 PGM (grayscale) image.
 *
 * Reads a P5 PGM from a file argument (or stdin when the argument is "-" or
 * absent), finds a continuous outer-frame marker, and prints the payload of
 * the first successfully decoded QR code which passes the minimal Waybeam
 * transport-envelope checks. --raw disables those checks for diagnostics.
 *
 * Robustness for real captures (perspective, small codes in a large frame,
 * rotation, mirroring):
 *   - required continuous outer-frame geometry supplies a direct projective
 *     transform for Version-1 symbols, without a rectified image;
 *   - each detected code is retried through quirc_flip() so mirrored/flipped
 *     codes decode (ISO 18004:2015 mirror feature);
 *   - measured low-cost full-frame and half-scale passes run first; radial
 *     correction handles strong fisheye curvature, and overlapping tiles stay
 *     late as the expensive small-code fallback.
 *   The first success wins.
 *
 * Exit codes:
 *   0  a QR code was decoded; payload written to stdout
 *   1  no QR code found / not decodable
 *   2  usage or input error (bad PGM, allocation failure)
 *
 * Pairs with waybeam's GET /api/v1/snapshot.pgm endpoint:
 *   curl -s http://127.0.0.1/api/v1/snapshot.pgm | qr_decode
 *
 * quirc (ISC) is vendored under tools/qr/quirc/.
 */

#include "quirc.h"
#include "waybeam_qr_format.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN_REGION   40   /* skip regions too small to hold a findable code */
#define MAX_INPUT_DIM 4096u
#define MAX_BOUNDED_ROIS 12
#define LENS_FALLBACK_K1 (-0.30)

struct decode_options {
	const char *path;
	int raw;
	int stats;
};

struct decode_stats {
	int enabled;
	int fatal_error;
	uint64_t started_us;
	uint64_t load_us;
	uint64_t prepare_us;
	uint64_t transform_us;
	uint64_t identify_us;
	uint64_t decode_us;
	unsigned regions;
	unsigned frame_candidates;
	unsigned refinements;
	unsigned finder_candidates;
	unsigned lens_corrections;
	unsigned qr_candidates;
	unsigned mirror_attempts;
	unsigned qr_decoded;
	unsigned envelope_rejected;
	char success_stage[32];
};

struct bounded_roi {
	int x;
	int y;
	int w;
	int h;
};

static void usage(FILE *f)
{
	fprintf(f,
		"usage: qr_decode [--raw] [--stats] [--] [FILE|-]\n"
		"\n"
		"Default: decode the first valid Waybeam Version-1/Q 16-character\n"
		"envelope inside the required continuous outer-frame profile.\n"
		"--raw emits any framed QR payload for bench diagnostics.\n"
		"--stats writes per-stage timings and decode status to stderr.\n");
}

static int parse_options(int argc, char **argv, struct decode_options *opts)
{
	int input_seen = 0;
	int options_done = 0;
	int i;

	memset(opts, 0, sizeof(*opts));

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!options_done && strcmp(arg, "--raw") == 0) {
			opts->raw = 1;
		} else if (!options_done && strcmp(arg, "--stats") == 0) {
			opts->stats = 1;
		} else if (!options_done && (strcmp(arg, "-h") == 0 ||
					    strcmp(arg, "--help") == 0)) {
			usage(stdout);
			return 1;
		} else if (!options_done && strcmp(arg, "--") == 0) {
			options_done = 1;
		} else if (strcmp(arg, "-") == 0 || options_done || arg[0] != '-') {
			if (input_seen) {
				fprintf(stderr, "qr_decode: only one input is allowed\n");
				return -1;
			}
			input_seen = 1;
			opts->path = strcmp(arg, "-") == 0 ? NULL : arg;
		} else {
			fprintf(stderr, "qr_decode: unknown option: %s\n", arg);
			return -1;
		}
	}
	return 0;
}

static uint64_t monotonic_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000u +
	       (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t stats_now(const struct decode_stats *stats)
{
	return stats->enabled ? monotonic_us() : 0;
}

static int allocation_failed(struct decode_stats *stats, const char *stage)
{
	if (!stats->fatal_error)
		fprintf(stderr, "qr_decode: allocation failed during %s\n", stage);
	stats->fatal_error = 1;
	return 0;
}

/* Read the next unsigned integer from a P5 header, skipping whitespace and
 * '#' comment lines.  Returns 0 on success, -1 on EOF/format error. */
static int pgm_read_uint(FILE *f, unsigned *out)
{
	int c;

	for (;;) {
		c = fgetc(f);
		if (c == EOF)
			return -1;
		if (c == '#') {
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		break;
	}
	if (c < '0' || c > '9')
		return -1;

	unsigned v = 0;
	while (c >= '0' && c <= '9') {
		unsigned digit = (unsigned)(c - '0');

		if (v > (UINT_MAX - digit) / 10u)
			return -1;
		v = v * 10u + digit;
		c = fgetc(f);
	}
	if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
		return -1;
	*out = v;
	return 0;
}

/* Load a binary P5 PGM into a freshly malloc'd 8-bit grayscale buffer.
 * Returns the pixel buffer (caller frees) and writes w/h, or NULL on error. */
static uint8_t *pgm_load(FILE *f, unsigned *w, unsigned *h)
{
	char magic[2] = {0};
	unsigned maxval = 0;

	if (fread(magic, 1, 2, f) != 2 || magic[0] != 'P' || magic[1] != '5') {
		fprintf(stderr, "qr_decode: not a P5 PGM\n");
		return NULL;
	}
	if (pgm_read_uint(f, w) != 0 || pgm_read_uint(f, h) != 0 ||
	    pgm_read_uint(f, &maxval) != 0) {
		fprintf(stderr, "qr_decode: malformed PGM header\n");
		return NULL;
	}
	if (*w == 0 || *h == 0 || *w > MAX_INPUT_DIM ||
	    *h > MAX_INPUT_DIM || maxval != 255) {
		fprintf(stderr, "qr_decode: unsupported PGM (w=%u h=%u max=%u; "
			"need dimensions 1..%u and maxval 255)\n",
			*w, *h, maxval, MAX_INPUT_DIM);
		return NULL;
	}
	/* Exactly one whitespace byte separates the header from the raster;
	 * pgm_read_uint already consumed it while scanning past maxval. */

	size_t npix = (size_t)*w * *h;
	uint8_t *pix = malloc(npix);
	if (!pix) {
		fprintf(stderr, "qr_decode: out of memory\n");
		return NULL;
	}
	if (fread(pix, 1, npix, f) != npix) {
		fprintf(stderr, "qr_decode: short pixel data\n");
		free(pix);
		return NULL;
	}
	return pix;
}

/* Run quirc over one w×h grayscale region copied out of src at (x0,y0), trying
 * every detected code both as-is and mirror-flipped.  Prints the first decoded
 * payload and returns 1; returns 0 if nothing decoded here. */
static int decode_candidates(struct quirc *q, int raw,
			     struct decode_stats *stats, const char *stage)
{
	int n = quirc_count(q);

	for (int i = 0; i < n; i++) {
		struct quirc_code code;
		struct quirc_data data;
		quirc_decode_error_t direct_err;
		quirc_decode_error_t mirror_err = QUIRC_SUCCESS;

		stats->qr_candidates++;
		quirc_extract(q, i, &code);
		direct_err = quirc_decode(&code, &data);
		if (direct_err != QUIRC_SUCCESS) {
			stats->mirror_attempts++;
			quirc_flip(&code);
			mirror_err = quirc_decode(&code, &data);
			if (mirror_err != QUIRC_SUCCESS) {
				if (stats->enabled)
					fprintf(stderr,
						"[qr-decode] stage=%s candidate=%d "
						"qr=failed direct=\"%s\" mirror=\"%s\"\n",
						stage, i,
						quirc_strerror(direct_err),
						quirc_strerror(mirror_err));
				continue;
			}
		}
		stats->qr_decoded++;
		if (!raw && !waybeam_qr_data_valid(&data)) {
			stats->envelope_rejected++;
			if (stats->enabled)
				fprintf(stderr,
					"[qr-decode] stage=%s candidate=%d "
					"qr=decoded envelope=rejected\n",
					stage, i);
			continue;
		}
		if (stats->enabled)
			fprintf(stderr,
				"[qr-decode] stage=%s candidate=%d "
				"qr=decoded envelope=%s\n",
				stage, i, raw ? "unchecked" : "accepted");
		fwrite(data.payload, 1, (size_t)data.payload_len, stdout);
		fputc('\n', stdout);
		snprintf(stats->success_stage, sizeof(stats->success_stage),
			 "%s", stage);
		return 1;
	}
	return 0;
}

/* Turn the QR corners projected from an accepted frame into a tight local
 * search window. The actual marker extends six units beyond each side of the
 * 21-unit QR; eight units leave tolerance for lens curvature and threshold
 * error without turning this into an unbounded finder scan. */
static int bounded_roi_from_code(const struct quirc_code *code,
				 int region_w, int region_h,
				 struct bounded_roi *roi)
{
	int min_x = code->corners[0].x;
	int max_x = min_x;
	int min_y = code->corners[0].y;
	int max_y = min_y;
	int i;
	int span_x;
	int span_y;
	int pad_x;
	int pad_y;

	for (i = 1; i < 4; i++) {
		if (code->corners[i].x < min_x)
			min_x = code->corners[i].x;
		if (code->corners[i].x > max_x)
			max_x = code->corners[i].x;
		if (code->corners[i].y < min_y)
			min_y = code->corners[i].y;
		if (code->corners[i].y > max_y)
			max_y = code->corners[i].y;
	}
	span_x = max_x - min_x + 1;
	span_y = max_y - min_y + 1;
	if (span_x < 16 || span_y < 16)
		return 0;
	pad_x = (span_x * 8 + 20) / 21;
	pad_y = (span_y * 8 + 20) / 21;
	min_x -= pad_x;
	max_x += pad_x;
	min_y -= pad_y;
	max_y += pad_y;
	if (min_x < 0)
		min_x = 0;
	if (min_y < 0)
		min_y = 0;
	if (max_x >= region_w)
		max_x = region_w - 1;
	if (max_y >= region_h)
		max_y = region_h - 1;

	roi->x = min_x;
	roi->y = min_y;
	roi->w = max_x - min_x + 1;
	roi->h = max_y - min_y + 1;
	return roi->w >= MIN_REGION && roi->h >= MIN_REGION;
}

/* Refine a QR only inside an ROI derived from accepted outer-frame geometry.
 * This uses standard finder patterns to absorb residual lens curvature, but
 * never runs unless marker-only discovery first supplied the bound. */
static int decode_bounded_refinement(struct quirc *q, const uint8_t *src,
		int sstride, int x0, int y0, const struct bounded_roi *roi,
		int raw, const char *stage, struct decode_stats *stats)
{
	uint8_t *buf;
	int r;
	int finder_candidates;
	int ok;
	unsigned decoded_before = stats->qr_decoded;
	unsigned rejected_before = stats->envelope_rejected;
	uint64_t started = stats_now(stats);
	uint64_t identify_started;
	uint64_t decode_started;
	uint64_t prepare_us;
	uint64_t identify_us;
	uint64_t decode_us;
	const char *result;

	if (quirc_resize(q, roi->w, roi->h) != 0)
		return allocation_failed(stats, stage);
	quirc_set_marker_mode(q, QUIRC_MARKER_OFF,
			      QUIRC_MARKER_PROFILE_NONE);
	buf = quirc_begin(q, NULL, NULL);
	for (r = 0; r < roi->h; r++)
		memcpy(buf + (size_t)r * roi->w,
		       src + (size_t)(y0 + roi->y + r) * sstride +
		       x0 + roi->x, (size_t)roi->w);
	identify_started = stats_now(stats);
	prepare_us = identify_started - started;
	quirc_end(q);
	decode_started = stats_now(stats);
	identify_us = decode_started - identify_started;
	finder_candidates = quirc_count(q);
	ok = decode_candidates(q, raw, stats, stage);
	decode_us = stats_now(stats) - decode_started;

	stats->refinements++;
	stats->finder_candidates += (unsigned)finder_candidates;
	stats->prepare_us += prepare_us;
	stats->identify_us += identify_us;
	stats->decode_us += decode_us;
	if (ok)
		result = "accepted";
	else if (finder_candidates == 0)
		result = "no-finder";
	else if (stats->qr_decoded > decoded_before &&
		 stats->envelope_rejected > rejected_before)
		result = "envelope-rejected";
	else
		result = "qr-failed";
	if (stats->enabled)
		fprintf(stderr,
			"[qr-decode] stage=%s roi=%d,%d,%dx%d "
			"prepare_us=%llu identify_us=%llu decode_us=%llu "
			"finders=%d result=%s\n",
			stage, x0 + roi->x, y0 + roi->y, roi->w, roi->h,
			(unsigned long long)prepare_us,
			(unsigned long long)identify_us,
			(unsigned long long)decode_us,
			finder_candidates, result);
	return ok;
}

/* Run bounded-frame discovery over one image region. Standards-only finder
 * discovery is deliberately never entered without an accepted outer frame.
 * Direct projective sampling is cheapest; a failed framed QR may then use
 * finder refinement inside the frame-derived ROI. */
static int decode_region(struct quirc *q, const uint8_t *src, int sstride,
		int x0, int y0, int w, int h, int raw, const char *stage,
		struct decode_stats *stats)
{
	uint8_t *buf;
	int bw = 0;
	int bh = 0;
	int r;
	int frame_candidates;
	int ok;
	int roi_count = 0;
	int i;
	struct bounded_roi rois[MAX_BOUNDED_ROIS];
	unsigned decoded_before;
	unsigned rejected_before;
	uint64_t started;
	uint64_t identify_started;
	uint64_t decode_started;
	uint64_t prepare_us;
	uint64_t identify_us;
	uint64_t decode_us;
	const char *result;

	if (w < MIN_REGION || h < MIN_REGION)
		return 0;
	started = stats_now(stats);
	if (quirc_resize(q, w, h) != 0)
		return allocation_failed(stats, stage);

	quirc_set_marker_mode(q, QUIRC_MARKER_ONLY,
			      QUIRC_MARKER_PROFILE_OUTER_FRAME_V1);
	buf = quirc_begin(q, &bw, &bh);
	for (r = 0; r < h; r++)
		memcpy(buf + (size_t)r * w,
		       src + (size_t)(y0 + r) * sstride + x0, (size_t)w);
	identify_started = stats_now(stats);
	prepare_us = identify_started - started;
	quirc_end(q);
	decode_started = stats_now(stats);
	identify_us = decode_started - identify_started;
	frame_candidates = quirc_count(q);
	for (i = 0; i < frame_candidates && roi_count < MAX_BOUNDED_ROIS; i++) {
		struct quirc_code code;

		quirc_extract(q, i, &code);
		if (bounded_roi_from_code(&code, w, h, &rois[roi_count]))
			roi_count++;
	}
	decoded_before = stats->qr_decoded;
	rejected_before = stats->envelope_rejected;
	ok = decode_candidates(q, raw, stats, stage);
	decode_us = stats_now(stats) - decode_started;

	stats->regions++;
	stats->frame_candidates += (unsigned)frame_candidates;
	stats->prepare_us += prepare_us;
	stats->identify_us += identify_us;
	stats->decode_us += decode_us;
	if (ok)
		result = "accepted";
	else if (frame_candidates == 0)
		result = "no-frame";
	else if (stats->qr_decoded > decoded_before &&
		 stats->envelope_rejected > rejected_before)
		result = "envelope-rejected";
	else
		result = "qr-failed";
	if (stats->enabled)
		fprintf(stderr,
			"[qr-decode] stage=%s region=%d,%d,%dx%d "
			"prepare_us=%llu identify_us=%llu decode_us=%llu "
			"frames=%d result=%s\n",
			stage, x0, y0, w, h,
			(unsigned long long)prepare_us,
			(unsigned long long)identify_us,
			(unsigned long long)decode_us,
			frame_candidates, result);
	if (ok || roi_count == 0)
		return ok;

	for (i = 0; i < roi_count; i++) {
		char refine_stage[32];

		snprintf(refine_stage, sizeof(refine_stage), "%s/refine", stage);
		if (decode_bounded_refinement(q, src, sstride, x0, y0,
					      &rois[i], raw, refine_stage,
					      stats))
			return 1;
		if (stats->fatal_error)
			return 0;
	}
	return 0;
}

/* 2×2 box-average downscale into a freshly malloc'd buffer (odd tail dropped).
 * Returns NULL if the result would be too small to bother. */
static uint8_t *downscale2(const uint8_t *src, int w, int h, int *ow, int *oh)
{
	int dw = w / 2, dh = h / 2;
	if (dw < MIN_REGION || dh < MIN_REGION)
		return NULL;

	uint8_t *dst = malloc((size_t)dw * dh);
	if (!dst)
		return NULL;
	for (int y = 0; y < dh; y++) {
		const uint8_t *r0 = src + (size_t)(2 * y) * w;
		const uint8_t *r1 = r0 + w;
		uint8_t *o = dst + (size_t)y * dw;
		for (int x = 0; x < dw; x++) {
			int s = r0[2 * x] + r0[2 * x + 1] +
				r1[2 * x] + r1[2 * x + 1];
			o[x] = (uint8_t)((s + 2) >> 2);
		}
	}
	*ow = dw;
	*oh = dh;
	return dst;
}

/* Three overlapping start offsets (0, centre, end) for one axis, or a single
 * offset when the axis is not worth tiling.  Returns the count (1 or up to 3,
 * de-duplicated). */
static int tile_offsets(int dim, int tile, int *out)
{
	int n = 0, cand[3], i, j;

	cand[0] = 0;
	cand[1] = (dim - tile) / 2;
	cand[2] = dim - tile;
	for (i = 0; i < 3; i++) {
		if (cand[i] < 0)
			cand[i] = 0;
		for (j = 0; j < n; j++)
			if (out[j] == cand[i])
				break;
		if (j == n)
			out[n++] = cand[i];
	}
	return n;
}

/* 3×3 box blur into a freshly malloc'd buffer (edges copied through).  A light
 * denoise lets a noisy capture's modules threshold cleanly — often the
 * difference between decode and no-decode on a small code (measured); over-
 * blurring merges modules, so exactly one pass is applied. */
static uint8_t *box_blur3(const uint8_t *src, int w, int h)
{
	uint8_t *dst = malloc((size_t)w * h);
	if (!dst)
		return NULL;
	memcpy(dst, src, (size_t)w * h);
	for (int y = 1; y < h - 1; y++) {
		for (int x = 1; x < w - 1; x++) {
			int s = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					s += src[(size_t)(y + dy) * w + (x + dx)];
			dst[(size_t)y * w + x] = (uint8_t)(s / 9);
		}
	}
	return dst;
}

/* Correct barrel distortion with the same radial model used by common image
 * tools:
 *
 *   source_radius = output_radius * (1 + k1*r^2)
 *
 * where r is normalised to the half-diagonal. A single measured coefficient
 * is deliberately a late fallback, not a camera-pipeline calibration. Nearest
 * sampling preserves hard QR module edges and keeps the implementation small.
 */
static uint8_t *lens_correct_radial(const uint8_t *src, int w, int h,
				    float k1)
{
	size_t n = (size_t)w * h;
	uint8_t *dst = malloc(n);
	float *x_terms = malloc((size_t)w * 2 * sizeof(*x_terms));
	float *x2;
	float *warp_x;
	float cx = (w - 1) * 0.5f;
	float cy = (h - 1) * 0.5f;
	float r2_scale = 4.0f /
		((float)w * w + (float)h * h);
	float alpha = k1 * r2_scale;
	int x;
	int y;

	if (!dst || !x_terms) {
		free(dst);
		free(x_terms);
		return NULL;
	}
	x2 = x_terms;
	warp_x = x2 + w;
	for (x = 0; x < w; x++) {
		float dx = x - cx;

		x2[x] = dx * dx;
		warp_x[x] = alpha * dx;
	}
	for (y = 0; y < h; y++) {
		float dy = y - cy;
		float dy2 = dy * dy;
		float warp_y = alpha * dy;

		for (x = 0; x < w; x++) {
			float radius2 = x2[x] + dy2;
			int sx = (int)((float)x +
				       warp_x[x] * radius2 + 0.5f);
			int sy = (int)((float)y +
				       warp_y * radius2 + 0.5f);

			if (sx >= 0 && sx < w && sy >= 0 && sy < h)
				dst[(size_t)y * w + x] =
					src[(size_t)sy * w + sx];
			else
				dst[(size_t)y * w + x] = 255;
		}
	}
	free(x_terms);
	return dst;
}

static int decode_full(struct quirc *q, const uint8_t *pix, int w, int h,
		       int raw, const char *variant,
		       struct decode_stats *stats)
{
	char stage[32];

	snprintf(stage, sizeof(stage), "%s/full", variant);
	return decode_region(q, pix, w, 0, 0, w, h, raw, stage, stats);
}

/* Overlapping half-frame tiles keep a small marker whole in at least one
 * locally thresholded region. */
static int decode_tiles(struct quirc *q, const uint8_t *pix, int w, int h,
			int raw, const char *variant,
			struct decode_stats *stats)
{
	char stage[32];

	/* Tile only when the frame is big enough that a small code benefits from
	 * a local, higher-fraction view.  Half-size tiles at 3 offsets per axis
	 * (50% overlap) so a code near a seam still lands whole in some tile. */
	if (w >= 2 * MIN_REGION && h >= 2 * MIN_REGION &&
	    (w > 640 || h > 640)) {
		int tw = (w + 1) / 2, th = (h + 1) / 2;
		int xs[3], ys[3];
		int nx = tile_offsets(w, tw, xs);
		int ny = tile_offsets(h, th, ys);
		snprintf(stage, sizeof(stage), "%s/tile", variant);
		for (int yi = 0; yi < ny; yi++) {
			for (int xi = 0; xi < nx; xi++) {
				if (decode_region(q, pix, w, xs[xi], ys[yi],
						  tw, th, raw, stage, stats))
					return 1;
				if (stats->fatal_error)
					return 0;
			}
		}
	}
	return 0;
}

/* One 2× downscaled full-frame attempt for a large or slightly soft marker. */
static int decode_half(struct quirc *q, const uint8_t *pix, int w, int h,
		       int raw, const char *variant,
		       struct decode_stats *stats)
{
	char stage[32];
	int hw = 0, hh = 0;
	uint64_t transform_started = stats_now(stats);

	if (w / 2 < MIN_REGION || h / 2 < MIN_REGION)
		return 0;
	uint8_t *half = downscale2(pix, w, h, &hw, &hh);
	stats->transform_us += stats_now(stats) - transform_started;
	if (!half)
		return allocation_failed(stats, "half-scale transform");

	snprintf(stage, sizeof(stage), "%s/half", variant);
	int ok = decode_region(q, half, hw, 0, 0, hw, hh, raw,
			       stage, stats);
	free(half);
	return ok;
}

/* One inverted full-frame pass.  Some codes are rendered light-on-dark (an
 * inverted display, a reversed print); quirc assumes dark-on-light and never
 * tries inversion itself, so a single 255-p pass covers them.  Kept to one
 * full-frame attempt — an inverted code that is also small/noisy is rare
 * enough not to warrant the full multi-pass strategy.  quirc already
 * normalises contrast/brightness via its adaptive threshold, so no
 * stretch/gamma pass is needed (measured: it decodes delta-10 contrast and
 * near-black frames unaided). */
static int decode_inverted(struct quirc *q, const uint8_t *pix, int w, int h,
			   int raw, struct decode_stats *stats)
{
	size_t n = (size_t)w * h;
	uint64_t transform_started = stats_now(stats);
	uint8_t *inv = malloc(n);
	if (!inv)
		return allocation_failed(stats, "inversion");
	for (size_t i = 0; i < n; i++)
		inv[i] = (uint8_t)(255 - pix[i]);
	stats->transform_us += stats_now(stats) - transform_started;
	int ok = decode_region(q, inv, w, 0, 0, w, h, raw,
			       "inverted/full", stats);
	free(inv);
	return ok;
}

static int decode_lens(struct quirc *q, const uint8_t *pix, int w, int h,
		       int raw, struct decode_stats *stats)
{
	uint8_t *corrected;
	uint64_t transform_started = stats_now(stats);

	corrected = lens_correct_radial(pix, w, h, LENS_FALLBACK_K1);
	stats->transform_us += stats_now(stats) - transform_started;
	if (!corrected)
		return allocation_failed(stats, "lens correction");

	stats->lens_corrections++;
	if (decode_full(q, corrected, w, h, raw, "lens", stats)) {
		free(corrected);
		return 1;
	}
	if (stats->fatal_error) {
		free(corrected);
		return 0;
	}

	transform_started = stats_now(stats);
	uint8_t *corrected_blur = box_blur3(corrected, w, h);
	stats->transform_us += stats_now(stats) - transform_started;
	if (!corrected_blur) {
		free(corrected);
		return allocation_failed(stats, "lens blur");
	}
	int ok = decode_full(q, corrected_blur, w, h, raw,
			     "lens-blur", stats);

	free(corrected_blur);
	if (ok) {
		free(corrected);
		return 1;
	}
	free(corrected);
	return 0;
}

/* Run the measured bounded cascade from the cheapest sharp/blur attempts
 * through half-scale, lens, tiles, and finally inversion. */
static int decode_image(struct quirc *q, const uint8_t *pix, int w, int h,
			int raw, struct decode_stats *stats)
{
	uint8_t *blur;
	uint64_t transform_started;

	if (decode_full(q, pix, w, h, raw, "sharp", stats))
		return 1;
	if (stats->fatal_error)
		return 0;

	transform_started = stats_now(stats);
	blur = box_blur3(pix, w, h);
	stats->transform_us += stats_now(stats) - transform_started;
	if (!blur)
		return allocation_failed(stats, "blur");
	if (decode_full(q, blur, w, h, raw, "blur", stats)) {
		free(blur);
		return 1;
	}
	if (stats->fatal_error) {
		free(blur);
		return 0;
	}

	if (decode_half(q, pix, w, h, raw, "sharp", stats)) {
		free(blur);
		return 1;
	}
	if (stats->fatal_error) {
		free(blur);
		return 0;
	}

	/* Strong fisheye curvature cannot be represented by the frame's direct
	 * four-corner homography. Pay for one measured radial correction only
	 * after sharp half-scale discovery failed. The corrected path remains
	 * frame-gated and avoids the tile/scale expansion. Blur-half remains
	 * available afterward, but need not delay the measured fisheye path. */
	if (decode_lens(q, pix, w, h, raw, stats)) {
		free(blur);
		return 1;
	}
	if (stats->fatal_error) {
		free(blur);
		return 0;
	}

	if (decode_half(q, blur, w, h, raw, "blur", stats)) {
		free(blur);
		return 1;
	}
	if (stats->fatal_error) {
		free(blur);
		return 0;
	}

	if (decode_tiles(q, pix, w, h, raw, "sharp", stats)) {
		free(blur);
		return 1;
	}
	if (stats->fatal_error) {
		free(blur);
		return 0;
	}
	if (decode_tiles(q, blur, w, h, raw, "blur", stats)) {
		free(blur);
		return 1;
	}
	if (stats->fatal_error) {
		free(blur);
		return 0;
	}
	free(blur);
	return decode_inverted(q, pix, w, h, raw, stats);
}

int main(int argc, char **argv)
{
	struct decode_options opts;
	struct decode_stats stats;
	int opt_rc = parse_options(argc, argv, &opts);
	if (opt_rc > 0)
		return 0;
	if (opt_rc < 0) {
		usage(stderr);
		return 2;
	}

	memset(&stats, 0, sizeof(stats));
	stats.enabled = opts.stats;
	stats.started_us = stats_now(&stats);
	const char *path = opts.path;
	FILE *f = path ? fopen(path, "rb") : stdin;
	if (!f) {
		fprintf(stderr, "qr_decode: cannot open %s\n", path);
		return 2;
	}

	unsigned w = 0, h = 0;
	uint64_t load_started = stats_now(&stats);
	uint8_t *pix = pgm_load(f, &w, &h);
	stats.load_us = stats_now(&stats) - load_started;
	if (path)
		fclose(f);
	if (!pix)
		return 2;
	if (stats.enabled)
		fprintf(stderr,
			"[qr-decode] input=%s size=%ux%u "
			"profile=outer-frame-v1\n",
			path ? path : "stdin", w, h);

	struct quirc *q = quirc_new();
	if (!q) {
		fprintf(stderr, "qr_decode: quirc_new failed\n");
		free(pix);
		return 2;
	}
	int decoded = decode_image(q, pix, (int)w, (int)h, opts.raw,
				   &stats);
	int rc = stats.fatal_error ? 2 : (decoded ? 0 : 1);

	quirc_destroy(q);
	free(pix);
	if (stats.enabled)
		fprintf(stderr,
			"[qr-decode] summary result=%s final_stage=%s "
			"total_us=%llu load_us=%llu prepare_us=%llu "
			"transform_us=%llu identify_us=%llu decode_us=%llu "
			"regions=%u frames=%u refinements=%u finders=%u "
			"lens_corrections=%u "
			"qr_candidates=%u "
			"mirror_attempts=%u qr_decoded=%u "
			"envelope_rejected=%u\n",
			rc == 0 ? "decoded" :
			(rc == 1 ? "no-decode" : "fatal-error"),
			stats.success_stage[0] ? stats.success_stage : "none",
			(unsigned long long)(monotonic_us() -
					     stats.started_us),
			(unsigned long long)stats.load_us,
			(unsigned long long)stats.prepare_us,
			(unsigned long long)stats.transform_us,
			(unsigned long long)stats.identify_us,
			(unsigned long long)stats.decode_us,
			stats.regions, stats.frame_candidates,
			stats.refinements, stats.finder_candidates,
			stats.lens_corrections,
			stats.qr_candidates, stats.mirror_attempts,
			stats.qr_decoded, stats.envelope_rejected);
	return rc;
}
