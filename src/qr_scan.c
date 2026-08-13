/* qr_scan.c — the bounded QR decode cascade.
 *
 * Extracted from tools/qr/qr_decode.c as the freestanding helper's decoding
 * core. The venc daemon invokes that helper instead of linking quirc into its
 * long-lived process. See include/qr_scan.h for the memory model.
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
 */

#include "qr_scan.h"

#include "quirc.h"
#include "waybeam_qr_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN_REGION   40   /* skip regions too small to hold a findable code */
#define MAX_BOUNDED_ROIS 12
#define LENS_FALLBACK_K1 (-0.30)

/* A quirc bump that grew the payload would silently truncate here. */
#if QR_SCAN_PAYLOAD_MAX != QUIRC_MAX_PAYLOAD
#error "QR_SCAN_PAYLOAD_MAX is out of sync with QUIRC_MAX_PAYLOAD"
#endif

struct QrScanCtx {
	struct quirc *q;
	int         (*abort_cb)(void *user);
	void         *abort_user;
	/* Scratch arena, one W*H, grown on demand and retained across attempts.
	 * Every transform in the cascade writes here; holding two of them at once
	 * is what used to push peak heap to 5x W*H. */
	uint8_t      *arena;
	size_t        arena_cap;
};

struct bounded_roi {
	int x;
	int y;
	int w;
	int h;
};

static uint64_t monotonic_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000u +
	       (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t stats_now(const QrScanStats *stats)
{
	return stats->enabled ? monotonic_us() : 0;
}

static int allocation_failed(QrScanStats *stats, const char *stage)
{
	if (!stats->fatal_error)
		fprintf(stderr, "qr_scan: allocation failed during %s\n", stage);
	stats->fatal_error = 1;
	return 0;
}

/* Checked at every region boundary.  Sets stats->aborted so a caller can tell
 * "asked to stop" from "looked everywhere and found nothing". */
static int scan_aborted(QrScanCtx *ctx, QrScanStats *stats)
{
	if (stats->aborted)
		return 1;
	if (ctx->abort_cb && ctx->abort_cb(ctx->abort_user)) {
		stats->aborted = 1;
		return 1;
	}
	return 0;
}

/* Any stage that must unwind now: a fatal allocation failure or an abort. */
static int scan_unwind(const QrScanStats *stats)
{
	return stats->fatal_error || stats->aborted;
}

/* Grab the shared scratch arena at >= n bytes.  Retained across attempts so a
 * scan window that runs seven cascades does not malloc W*H fourteen times. */
static uint8_t *arena_get(QrScanCtx *ctx, size_t n)
{
	if (ctx->arena_cap < n) {
		uint8_t *nb = realloc(ctx->arena, n);

		if (!nb)
			return NULL;
		ctx->arena = nb;
		ctx->arena_cap = n;
	}
	return ctx->arena;
}

QrScanCtx *qr_scan_ctx_new(void)
{
	QrScanCtx *ctx = calloc(1, sizeof(*ctx));

	if (!ctx)
		return NULL;
	ctx->q = quirc_new();
	if (!ctx->q) {
		free(ctx);
		return NULL;
	}
	return ctx;
}

void qr_scan_ctx_free(QrScanCtx *ctx)
{
	if (!ctx)
		return;
	if (ctx->q)
		quirc_destroy(ctx->q);
	free(ctx->arena);
	free(ctx);
}

void qr_scan_set_abort(QrScanCtx *ctx, int (*cb)(void *user), void *user)
{
	if (!ctx)
		return;
	ctx->abort_cb = cb;
	ctx->abort_user = user;
}

/* Run quirc over one w×h grayscale region copied out of src at (x0,y0), trying
 * every detected code both as-is and mirror-flipped.  Records the first
 * accepted payload and returns 1; returns 0 if nothing decoded here. */
static int decode_candidates(QrScanCtx *ctx, int raw, QrScanResult *out,
			     QrScanStats *stats, const char *stage)
{
	int n = quirc_count(ctx->q);

	for (int i = 0; i < n; i++) {
		struct quirc_code code;
		struct quirc_data data;
		quirc_decode_error_t direct_err;
		quirc_decode_error_t mirror_err = QUIRC_SUCCESS;
		unsigned len;

		stats->qr_candidates++;
		quirc_extract(ctx->q, i, &code);
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
		len = data.payload_len < 0 ? 0u : (unsigned)data.payload_len;
		if (len > QR_SCAN_PAYLOAD_MAX)
			len = QR_SCAN_PAYLOAD_MAX;
		memcpy(out->payload, data.payload, len);
		out->payload_len = len;
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
static int decode_bounded_refinement(QrScanCtx *ctx, const uint8_t *src,
		int sstride, int x0, int y0, const struct bounded_roi *roi,
		int raw, const char *stage, QrScanResult *out,
		QrScanStats *stats)
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

	if (scan_aborted(ctx, stats))
		return 0;
	if (quirc_resize(ctx->q, roi->w, roi->h) != 0)
		return allocation_failed(stats, stage);
	quirc_set_marker_mode(ctx->q, QUIRC_MARKER_OFF,
			      QUIRC_MARKER_PROFILE_NONE);
	buf = quirc_begin(ctx->q, NULL, NULL);
	for (r = 0; r < roi->h; r++)
		memcpy(buf + (size_t)r * roi->w,
		       src + (size_t)(y0 + roi->y + r) * sstride +
		       x0 + roi->x, (size_t)roi->w);
	identify_started = stats_now(stats);
	prepare_us = identify_started - started;
	quirc_end(ctx->q);
	decode_started = stats_now(stats);
	identify_us = decode_started - identify_started;
	finder_candidates = quirc_count(ctx->q);
	ok = decode_candidates(ctx, raw, out, stats, stage);
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
static int decode_region(QrScanCtx *ctx, const uint8_t *src, int sstride,
		int x0, int y0, int w, int h, int raw, const char *stage,
		QrScanResult *out, QrScanStats *stats)
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
	if (scan_aborted(ctx, stats))
		return 0;
	started = stats_now(stats);
	if (quirc_resize(ctx->q, w, h) != 0)
		return allocation_failed(stats, stage);

	quirc_set_marker_mode(ctx->q, QUIRC_MARKER_ONLY,
			      QUIRC_MARKER_PROFILE_OUTER_FRAME_V1);
	buf = quirc_begin(ctx->q, &bw, &bh);
	for (r = 0; r < h; r++)
		memcpy(buf + (size_t)r * w,
		       src + (size_t)(y0 + r) * sstride + x0, (size_t)w);
	identify_started = stats_now(stats);
	prepare_us = identify_started - started;
	quirc_end(ctx->q);
	decode_started = stats_now(stats);
	identify_us = decode_started - identify_started;
	frame_candidates = quirc_count(ctx->q);
	for (i = 0; i < frame_candidates && roi_count < MAX_BOUNDED_ROIS; i++) {
		struct quirc_code code;

		quirc_extract(ctx->q, i, &code);
		if (bounded_roi_from_code(&code, w, h, &rois[roi_count]))
			roi_count++;
	}
	decoded_before = stats->qr_decoded;
	rejected_before = stats->envelope_rejected;
	ok = decode_candidates(ctx, raw, out, stats, stage);
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
		if (decode_bounded_refinement(ctx, src, sstride, x0, y0,
					      &rois[i], raw, refine_stage,
					      out, stats))
			return 1;
		if (scan_unwind(stats))
			return 0;
	}
	return 0;
}

/* 2×2 box-average downscale into `dst` (odd tail dropped).  `dst` must hold at
 * least (w/2)*(h/2) bytes and must not overlap `src`.  Returns 0, or -1 when
 * the result would be too small to bother with. */
static int downscale2(const uint8_t *src, int w, int h, uint8_t *dst,
		      int *ow, int *oh)
{
	int dw = w / 2, dh = h / 2;

	if (dw < MIN_REGION || dh < MIN_REGION)
		return -1;
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
	return 0;
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

/* 3×3 box blur, in place, edges copied through.  A light denoise lets a noisy
 * capture's modules threshold cleanly — often the difference between decode and
 * no-decode on a small code (measured); over-blurring merges modules, so
 * exactly one pass is applied.
 *
 * In place because holding a separate destination is a whole W*H of peak heap,
 * and this is a 3×3 kernel: output row y depends only on input rows y-1..y+1.
 * Rows y+1 downward are still pristine in `buf`; row y-1 has already been
 * overwritten, so a two-row ring carries the originals forward.  Bit-identical
 * to the allocating form — pinned by test_qr_scan.c, not by inspection. */
int qr_scan_box_blur3_inplace(uint8_t *buf, int w, int h)
{
	uint8_t *ring;

	if (w < 3 || h < 3)
		return 0;   /* all edge, nothing to filter */
	ring = malloc((size_t)w * 2);
	if (!ring)
		return -1;

	memcpy(ring, buf, (size_t)w);   /* original row 0 */
	for (int y = 1; y < h - 1; y++) {
		uint8_t *prev = ring + (size_t)((y - 1) & 1) * w;
		uint8_t *cur  = ring + (size_t)(y & 1) * w;
		const uint8_t *next = buf + (size_t)(y + 1) * w;
		uint8_t *o = buf + (size_t)y * w;

		memcpy(cur, o, (size_t)w);   /* stash row y before overwriting */
		for (int x = 1; x < w - 1; x++) {
			int s = prev[x - 1] + prev[x] + prev[x + 1] +
				cur[x - 1]  + cur[x]  + cur[x + 1] +
				next[x - 1] + next[x] + next[x + 1];
			o[x] = (uint8_t)(s / 9);
		}
	}
	free(ring);
	return 0;
}

/* Correct barrel distortion with the same radial model used by common image
 * tools:
 *
 *   source_radius = output_radius * (1 + k1*r^2)
 *
 * where r is normalised to the half-diagonal. A single measured coefficient
 * is deliberately a late fallback, not a camera-pipeline calibration. Nearest
 * sampling preserves hard QR module edges and keeps the implementation small.
 *
 * Writes into `dst`, which must not overlap `src` (samples are gathered from
 * arbitrary offsets, so this one genuinely cannot run in place).
 */
static int lens_correct_radial(const uint8_t *src, int w, int h, float k1,
			       uint8_t *dst)
{
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

	if (!x_terms)
		return -1;
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
	return 0;
}

static int decode_full(QrScanCtx *ctx, const uint8_t *pix, int w, int h,
		       int raw, const char *variant, QrScanResult *out,
		       QrScanStats *stats)
{
	char stage[32];

	snprintf(stage, sizeof(stage), "%s/full", variant);
	return decode_region(ctx, pix, w, 0, 0, w, h, raw, stage, out, stats);
}

/* Overlapping half-frame tiles keep a small marker whole in at least one
 * locally thresholded region. */
static int decode_tiles(QrScanCtx *ctx, const uint8_t *pix, int w, int h,
			int raw, const char *variant, QrScanResult *out,
			QrScanStats *stats)
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
				if (decode_region(ctx, pix, w, xs[xi], ys[yi],
						  tw, th, raw, stage, out,
						  stats))
					return 1;
				if (scan_unwind(stats))
					return 0;
			}
		}
	}
	return 0;
}

/* One 2× downscaled full-frame attempt for a large or slightly soft marker.
 * The half image lands in the back QUARTER of the arena, so it can coexist with
 * a full-size transform occupying the front. */
static int decode_half(QrScanCtx *ctx, const uint8_t *pix, int w, int h,
		       int raw, const char *variant, uint8_t *scratch,
		       QrScanResult *out, QrScanStats *stats)
{
	char stage[32];
	int hw = 0, hh = 0;
	uint64_t transform_started = stats_now(stats);
	int rc;

	if (w / 2 < MIN_REGION || h / 2 < MIN_REGION)
		return 0;
	rc = downscale2(pix, w, h, scratch, &hw, &hh);
	stats->transform_us += stats_now(stats) - transform_started;
	if (rc != 0)
		return 0;

	snprintf(stage, sizeof(stage), "%s/half", variant);
	return decode_region(ctx, scratch, hw, 0, 0, hw, hh, raw, stage, out,
			     stats);
}

/* One inverted full-frame pass.  Some codes are rendered light-on-dark (an
 * inverted display, a reversed print); quirc assumes dark-on-light and never
 * tries inversion itself, so a single 255-p pass covers them.  Kept to one
 * full-frame attempt — an inverted code that is also small/noisy is rare
 * enough not to warrant the full multi-pass strategy.  quirc already
 * normalises contrast/brightness via its adaptive threshold, so no
 * stretch/gamma pass is needed (measured: it decodes delta-10 contrast and
 * near-black frames unaided). */
static int decode_inverted(QrScanCtx *ctx, const uint8_t *pix, int w, int h,
			   int raw, uint8_t *scratch, QrScanResult *out,
			   QrScanStats *stats)
{
	size_t n = (size_t)w * h;
	uint64_t transform_started = stats_now(stats);

	for (size_t i = 0; i < n; i++)
		scratch[i] = (uint8_t)(255 - pix[i]);
	stats->transform_us += stats_now(stats) - transform_started;
	return decode_region(ctx, scratch, w, 0, 0, w, h, raw,
			     "inverted/full", out, stats);
}

static int decode_lens(QrScanCtx *ctx, const uint8_t *pix, int w, int h,
		       int raw, uint8_t *scratch, QrScanResult *out,
		       QrScanStats *stats)
{
	uint64_t transform_started = stats_now(stats);
	int rc;

	rc = lens_correct_radial(pix, w, h, LENS_FALLBACK_K1, scratch);
	stats->transform_us += stats_now(stats) - transform_started;
	if (rc != 0)
		return allocation_failed(stats, "lens correction");

	stats->lens_corrections++;
	if (decode_full(ctx, scratch, w, h, raw, "lens", out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;

	/* Blur the correction in place: the unblurred version has had its turn
	 * and nothing downstream reads it again. */
	transform_started = stats_now(stats);
	rc = qr_scan_box_blur3_inplace(scratch, w, h);
	stats->transform_us += stats_now(stats) - transform_started;
	if (rc != 0)
		return allocation_failed(stats, "lens blur");

	return decode_full(ctx, scratch, w, h, raw, "lens-blur", out, stats);
}

/* Blur `pix` into the scratch arena. */
static int blur_into(const uint8_t *pix, int w, int h, uint8_t *scratch,
		     QrScanStats *stats)
{
	uint64_t transform_started = stats_now(stats);
	int rc;

	memcpy(scratch, pix, (size_t)w * h);
	rc = qr_scan_box_blur3_inplace(scratch, w, h);
	stats->transform_us += stats_now(stats) - transform_started;
	return rc;
}

/* Run the measured bounded cascade from the cheapest sharp/blur attempts
 * through half-scale, lens, tiles, and finally inversion.
 *
 * Stage ORDER is measured and load-bearing; do not reorder to suit the arena.
 * What the arena changes is only WHO owns the scratch at each point: the blur
 * is recomputed after the lens stage rather than held across it, which trades
 * one extra ~30 ms blur pass on the deep-failure path for a whole W*H of peak
 * heap.  On a 90 MB device that is the right side of the trade. */
int qr_scan_image(QrScanCtx *ctx, const uint8_t *pix, int w, int h, int raw,
		  QrScanResult *out, QrScanStats *stats)
{
	uint8_t *scratch;
	uint8_t *half;

	if (!ctx || !pix || !out || !stats || w <= 0 || h <= 0)
		return 0;
	out->payload_len = 0;

	if (decode_full(ctx, pix, w, h, raw, "sharp", out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;

	/* One allocation for the whole cascade: a full-size transform slot plus
	 * a quarter-size half-scale slot behind it.  Taken in a single call so
	 * no later growth can move the arena under a live pointer. */
	scratch = arena_get(ctx, (size_t)w * h + (size_t)(w / 2) * (h / 2));
	if (!scratch)
		return allocation_failed(stats, "scratch arena");
	half = scratch + (size_t)w * h;

	if (blur_into(pix, w, h, scratch, stats) != 0)
		return allocation_failed(stats, "blur");
	if (decode_full(ctx, scratch, w, h, raw, "blur", out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;

	if (decode_half(ctx, pix, w, h, raw, "sharp", half, out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;

	/* Strong fisheye curvature cannot be represented by the frame's direct
	 * four-corner homography. Pay for one measured radial correction only
	 * after sharp half-scale discovery failed. The corrected path remains
	 * frame-gated and avoids the tile/scale expansion. Blur-half remains
	 * available afterward, but need not delay the measured fisheye path.
	 *
	 * This overwrites the blur; it is recomputed below. */
	if (decode_lens(ctx, pix, w, h, raw, scratch, out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;

	if (blur_into(pix, w, h, scratch, stats) != 0)
		return allocation_failed(stats, "blur");

	if (decode_half(ctx, scratch, w, h, raw, "blur", half, out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;

	if (decode_tiles(ctx, pix, w, h, raw, "sharp", out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;
	if (decode_tiles(ctx, scratch, w, h, raw, "blur", out, stats))
		return 1;
	if (scan_unwind(stats))
		return 0;

	/* The blur is dead now; reuse its bytes for the inversion. */
	return decode_inverted(ctx, pix, w, h, raw, scratch, out, stats);
}
