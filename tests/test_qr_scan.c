/* test_qr_scan.c — regression cover for the shared QR decode cascade.
 *
 * Why this file exists: before it, the only test that ran the cascade was
 * tests/test_qr_cli.sh, which decodes ONE fixture that wins on the very first
 * stage.  Everything past "sharp/full" — blur, half-scale, lens correction,
 * tiling, inversion — had no coverage at all, which is precisely the code the
 * arena/in-place-blur rework rewrites.  The 768-render corpus in
 * test_qr_marker.c does not help: it drives quirc directly and never enters
 * qr_scan_image().
 *
 * Two things are pinned here:
 *
 *   1. box_blur3 in place is BIT-IDENTICAL to the allocating reference.  A
 *      subtle error in the two-row ring would not crash; it would quietly
 *      degrade decode rate on noisy captures, so it needs an exact assertion
 *      rather than an eyeball.
 *
 *   2. Each cascade stage still wins the cases it is supposed to win.  Stage
 *      identity is the assertion — "it decoded" would pass even if a broken
 *      lens pass were being rescued by tiling three stages later.
 */

#include "qr_scan.h"
#include "qr_marker_render.h"
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

/* The allocating 3×3 box blur exactly as it stood before the in-place rework.
 * Kept here, in the test, as the reference the optimised version is measured
 * against. */
static uint8_t *box_blur3_reference(const uint8_t *src, int w, int h)
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

static int blur_matches_reference(int w, int h, uint32_t seed)
{
	size_t n = (size_t)w * h;
	uint8_t *src = malloc(n);
	uint8_t *ref;
	uint8_t *in_place = malloc(n);
	int ok;

	if (!src || !in_place) {
		free(src);
		free(in_place);
		return 0;
	}
	for (size_t i = 0; i < n; i++)
		src[i] = (uint8_t)(qr_rng_next(&seed) >> 24);
	memcpy(in_place, src, n);

	ref = box_blur3_reference(src, w, h);
	ok = ref && qr_scan_box_blur3_inplace(in_place, w, h) == 0 &&
	     memcmp(ref, in_place, n) == 0;

	free(src);
	free(ref);
	free(in_place);
	return ok;
}

/* --- cascade fixtures ------------------------------------------------- */

struct scan_image {
	uint8_t *pix;
	int      w, h;
};

static void scan_image_free(struct scan_image *im)
{
	free(im->pix);
	im->pix = NULL;
}

static int scan_image_alloc(struct scan_image *im, int w, int h)
{
	im->pix = malloc((size_t)w * h);
	im->w = w;
	im->h = h;
	return im->pix != NULL;
}

/* Salt-and-pepper at density/255.  High-frequency by construction, so it
 * defeats the sharp pass while a single 3×3 average recovers the modules —
 * which is exactly what the blur stage is for. */
static void add_impulse_noise(struct scan_image *im, unsigned density,
	uint32_t seed)
{
	size_t n = (size_t)im->w * im->h;

	for (size_t i = 0; i < n; i++) {
		uint32_t r = qr_rng_next(&seed);

		if ((r & 0xFFu) < density)
			im->pix[i] = (r & 0x100u) ? 255 : 0;
	}
}

/* The same radial model the cascade's lens stage corrects for, applied with
 * the opposite sign so the correction has something to undo. */
static int apply_barrel(struct scan_image *im, float k)
{
	int w = im->w, h = im->h;
	uint8_t *dst = malloc((size_t)w * h);
	float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
	float scale = 4.0f / ((float)w * w + (float)h * h);

	if (!dst)
		return 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float dx = x - cx, dy = y - cy;
			float r2 = (dx * dx + dy * dy) * scale;
			int sx = (int)(x + dx * (k * r2) + 0.5f);
			int sy = (int)(y + dy * (k * r2) + 0.5f);

			dst[(size_t)y * w + x] =
				(sx >= 0 && sx < w && sy >= 0 && sy < h)
					? im->pix[(size_t)sy * w + sx] : 255;
		}
	}
	free(im->pix);
	im->pix = dst;
	return 1;
}

static void invert(struct scan_image *im)
{
	size_t n = (size_t)im->w * im->h;

	for (size_t i = 0; i < n; i++)
		im->pix[i] = (uint8_t)(255 - im->pix[i]);
}

/* Run the cascade and report the winning stage.  `stage_out` is "" when
 * nothing decoded. */
static int run_scan(QrScanCtx *ctx, const struct scan_image *im,
	char *stage_out, size_t stage_len, QrScanStats *stats_out)
{
	QrScanResult res;
	QrScanStats stats;
	int ok;

	memset(&res, 0, sizeof(res));
	memset(&stats, 0, sizeof(stats));
	ok = qr_scan_image(ctx, im->pix, im->w, im->h, 0, &res, &stats);
	snprintf(stage_out, stage_len, "%s", stats.success_stage);
	if (stats_out)
		*stats_out = stats;
	if (!ok)
		return 0;
	return res.payload_len == strlen(QR_EXPECTED_PAYLOAD) &&
	       memcmp(res.payload, QR_EXPECTED_PAYLOAD, res.payload_len) == 0;
}

/* Does the winning stage start with `prefix`?  Prefix rather than equality
 * because a stage may legitimately win either directly or through its bounded
 * refinement ("lens/full" vs "lens/full/refine") — both mean the lens pass did
 * the work, which is what is being asserted. */
static int stage_is(const char *stage, const char *prefix)
{
	return strncmp(stage, prefix, strlen(prefix)) == 0;
}

static int abort_always(void *user)
{
	(void)user;
	return 1;
}

static int abort_after_n(void *user)
{
	int *left = user;

	if (*left <= 0)
		return 1;
	(*left)--;
	return 0;
}

int test_qr_scan(void)
{
	int failures = 0;
	QrScanCtx *ctx;
	struct scan_image im;
	char stage[32];
	QrScanStats stats;

	printf("\n=== qr_scan ===\n");

	/* --- 1. in-place blur is bit-identical to the reference ---------- */
	CHECK("blur_inplace_matches_reference_64x64",
	      blur_matches_reference(64, 64, 0x12345678u));
	CHECK("blur_inplace_matches_reference_65x33",
	      blur_matches_reference(65, 33, 0x9E3779B9u));
	CHECK("blur_inplace_matches_reference_3x3",
	      blur_matches_reference(3, 3, 0xDEADBEEFu));
	CHECK("blur_inplace_matches_reference_wide",
	      blur_matches_reference(257, 5, 0x0BADC0DEu));
	CHECK("blur_inplace_matches_reference_tall",
	      blur_matches_reference(5, 257, 0xFEEDFACEu));
	/* Degenerate shapes are all edge: the reference leaves them untouched
	 * and so must the in-place form. */
	CHECK("blur_inplace_matches_reference_2x2",
	      blur_matches_reference(2, 2, 0x5A5A5A5Au));
	CHECK("blur_inplace_matches_reference_1x9",
	      blur_matches_reference(1, 9, 0xA5A5A5A5u));

	ctx = qr_scan_ctx_new();
	CHECK("ctx_new", ctx != NULL);
	if (!ctx)
		return failures;

	/* --- 2. stage identity ------------------------------------------- */

	/* Clean marker: cheapest stage must win.  If this ever regresses to a
	 * later stage the cascade is doing needless work on the common case. */
	CHECK("sharp_image_alloc", scan_image_alloc(&im, 700, 700));
	qr_render_square(im.pix, im.w, im.h, 300);
	CHECK("sharp_decodes", run_scan(ctx, &im, stage, sizeof(stage), NULL));
	CHECK("sharp_wins_sharp_full", stage_is(stage, "sharp/full"));
	scan_image_free(&im);

	/* Impulse noise: sharp fails, one blur pass recovers it. */
	CHECK("blur_image_alloc", scan_image_alloc(&im, 700, 700));
	qr_render_square(im.pix, im.w, im.h, 300);
	add_impulse_noise(&im, 30, 0x1234u);
	CHECK("blur_decodes", run_scan(ctx, &im, stage, sizeof(stage), NULL));
	CHECK("blur_wins_blur_stage", stage_is(stage, "blur/"));
	scan_image_free(&im);

	/* Light-on-dark: only the inversion pass can reach it, and it is last,
	 * so this also proves the whole cascade ran without unwinding. */
	CHECK("inverted_image_alloc", scan_image_alloc(&im, 700, 700));
	qr_render_square(im.pix, im.w, im.h, 300);
	invert(&im);
	CHECK("inverted_decodes",
	      run_scan(ctx, &im, stage, sizeof(stage), NULL));
	CHECK("inverted_wins_inverted_full", stage_is(stage, "inverted/full"));
	scan_image_free(&im);

	/* Big marker under heavy impulse noise: neither full-res pass survives,
	 * and the 2× box average is what recovers the modules. */
	CHECK("half_image_alloc", scan_image_alloc(&im, 650, 650));
	qr_render_square(im.pix, im.w, im.h, 500);
	add_impulse_noise(&im, 120, 0x77u);
	CHECK("half_decodes", run_scan(ctx, &im, stage, sizeof(stage), NULL));
	CHECK("half_wins_half_stage", strstr(stage, "/half") != NULL);
	scan_image_free(&im);

	/* Strong barrel distortion: the frame's four-corner homography cannot
	 * represent it, so the radial correction has to earn the decode.  k is
	 * mid-plateau (measured: 0.8-1.3 all land on lens/full), not on an edge
	 * where a harmless numeric change would flip the stage. */
	CHECK("lens_image_alloc", scan_image_alloc(&im, 900, 900));
	qr_render_square(im.pix, im.w, im.h, 380);
	CHECK("lens_warp", apply_barrel(&im, 1.1f));
	CHECK("lens_decodes", run_scan(ctx, &im, stage, sizeof(stage), NULL));
	CHECK("lens_wins_lens_stage", stage_is(stage, "lens/"));
	scan_image_free(&im);

	/* Past the plateau the unblurred correction no longer decodes and the
	 * blurred one does — the only case that exercises blurring a transform
	 * in place on top of itself. */
	CHECK("lens_blur_image_alloc", scan_image_alloc(&im, 900, 900));
	qr_render_square(im.pix, im.w, im.h, 380);
	CHECK("lens_blur_warp", apply_barrel(&im, 1.4f));
	CHECK("lens_blur_decodes",
	      run_scan(ctx, &im, stage, sizeof(stage), NULL));
	CHECK("lens_blur_wins_lens_blur", stage_is(stage, "lens-blur/"));
	scan_image_free(&im);

	/* Not covered by a win: the tile stages.  Synthetic renders that defeat
	 * full, half and lens also defeat tiling, so no fixture reaches them
	 * first.  They still RUN — the exhaustion case below walks through them
	 * — which is what the arena rework needed covered. */

	/* --- 3. exhaustion: no code at all --------------------------------
	 * The most important memory path — every transform allocates, every
	 * stage runs, nothing short-circuits.  Must end clean, not fatal. */
	CHECK("noise_image_alloc", scan_image_alloc(&im, 800, 800));
	{
		uint32_t seed = 0xC0FFEEu;

		for (size_t i = 0; i < (size_t)im.w * im.h; i++)
			im.pix[i] = (uint8_t)(qr_rng_next(&seed) >> 24);
	}
	CHECK("pure_noise_does_not_decode",
	      !run_scan(ctx, &im, stage, sizeof(stage), &stats));
	CHECK("pure_noise_not_fatal", stats.fatal_error == 0);
	CHECK("pure_noise_not_aborted", stats.aborted == 0);
	/* Every stage really ran: sharp+blur+half×2+lens×2+tiles+inverted is
	 * far more than a handful of regions. */
	CHECK("pure_noise_ran_full_cascade", stats.regions >= 8);
	CHECK("pure_noise_ran_lens", stats.lens_corrections == 1);
	scan_image_free(&im);

	/* --- 4. abort callback -------------------------------------------
	 * This is what lets a scan window honour its deadline and answer
	 * /qr/stop without waiting out a full cascade. */
	CHECK("abort_image_alloc", scan_image_alloc(&im, 800, 800));
	{
		uint32_t seed = 0xC0FFEEu;

		for (size_t i = 0; i < (size_t)im.w * im.h; i++)
			im.pix[i] = (uint8_t)(qr_rng_next(&seed) >> 24);
	}
	qr_scan_set_abort(ctx, abort_always, NULL);
	CHECK("abort_immediate_returns_no_decode",
	      !run_scan(ctx, &im, stage, sizeof(stage), &stats));
	CHECK("abort_immediate_sets_aborted", stats.aborted == 1);
	CHECK("abort_immediate_ran_nothing", stats.regions == 0);
	CHECK("abort_immediate_not_fatal", stats.fatal_error == 0);

	{
		int budget = 3;

		qr_scan_set_abort(ctx, abort_after_n, &budget);
		CHECK("abort_midway_returns_no_decode",
		      !run_scan(ctx, &im, stage, sizeof(stage), &stats));
		CHECK("abort_midway_sets_aborted", stats.aborted == 1);
		/* Stopped early — nowhere near the 8+ regions of a full run. */
		CHECK("abort_midway_stopped_early", stats.regions <= 3);
	}
	qr_scan_set_abort(ctx, NULL, NULL);

	/* Cleared callback: the same context decodes normally again, proving
	 * an aborted attempt leaves no sticky state behind. */
	scan_image_free(&im);
	CHECK("reuse_image_alloc", scan_image_alloc(&im, 700, 700));
	qr_render_square(im.pix, im.w, im.h, 300);
	CHECK("ctx_reusable_after_abort",
	      run_scan(ctx, &im, stage, sizeof(stage), NULL));
	scan_image_free(&im);

	/* --- 5. defensive input ------------------------------------------ */
	{
		QrScanResult res;
		QrScanStats st;
		uint8_t one = 0;

		memset(&res, 0, sizeof(res));
		memset(&st, 0, sizeof(st));
		CHECK("zero_dims_rejected",
		      qr_scan_image(ctx, &one, 0, 0, 0, &res, &st) == 0);
		CHECK("null_pix_rejected",
		      qr_scan_image(ctx, NULL, 10, 10, 0, &res, &st) == 0);
		CHECK("negative_dims_rejected",
		      qr_scan_image(ctx, &one, -4, 4, 0, &res, &st) == 0);
		/* Below MIN_REGION: no region is worth scanning, but it must
		 * return cleanly rather than trip a bounds check. */
		CHECK("tiny_image_clean",
		      qr_scan_image(ctx, &one, 1, 1, 0, &res, &st) == 0 &&
		      st.fatal_error == 0);
	}

	qr_scan_ctx_free(ctx);
	qr_scan_ctx_free(NULL);   /* must tolerate NULL */
	return failures;
}
