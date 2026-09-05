#include "pipeline_common.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "venc_config.h"
#include <sys/stat.h>
#include <unistd.h>

/* Create an empty file at path; returns 1 on success.  Used for fixture
 * setup in the resolve_isp_bin tests. */
static int touch_file(const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}


/* Drive the watch one window at a time.
 *
 * The first version of this fed N seconds of 10 ms frames and let the 2 s
 * boundary fall where it may.  The feed lengths did not line up with the
 * window, so a "one window at 2x" arm actually straddled two windows and
 * diluted below the trip threshold -- every assertion passed, and so did
 * mutants with the two-window rule, the hysteresis and the zero-target guard
 * all removed.  Each call here closes EXACTLY one window at EXACTLY the
 * requested rate, so the assertions are pinned to window counts. */
static void rw_prime(PipelineRateWatch *rw, const VencConfig *cfg, uint64_t *t)
{
	pipeline_common_rate_watch(rw, cfg, 0, *t);   /* opens the window */
}

static void rw_window(PipelineRateWatch *rw, const VencConfig *cfg,
	uint32_t kbps, uint64_t *t)
{
	/* bytes carrying `kbps` for one 2 s window = kbps * 1000 * 2 / 8. */
	uint32_t bytes = (uint32_t)((uint64_t)kbps * 250ULL);

	*t += 2000000ULL;
	pipeline_common_rate_watch(rw, cfg, bytes, *t);
}

static int test_rate_watch(void)
{
	int failures = 0;
	VencConfig cfg;
	PipelineRateWatch rw;
	uint64_t t = 1000000ULL;

	venc_config_defaults(&cfg);
	cfg.video0.bitrate = 4000;

	/* At target: never trips, however long it runs. */
	memset(&rw, 0, sizeof(rw));
	rw_prime(&rw, &cfg, &t);
	for (int i = 0; i < 6; i++)
		rw_window(&rw, &cfg, 4000, &t);
	CHECK("rate_watch_at_target_quiet", rw.reported == 0);
	CHECK("rate_watch_at_target_no_streak", rw.over_windows == 0);

	/* ONE window at 2x is not enough -- the rule the two-window requirement
	 * exists for.  A moving scene produced 1.43x for a single window on the
	 * bench and reporting that would make the warning noise. */
	memset(&rw, 0, sizeof(rw));
	rw_prime(&rw, &cfg, &t);
	rw_window(&rw, &cfg, 4000, &t);
	rw_window(&rw, &cfg, 8000, &t);
	CHECK("rate_watch_one_over_window_quiet", rw.reported == 0);
	CHECK("rate_watch_one_over_window_counted", rw.over_windows == 1);
	rw_window(&rw, &cfg, 4000, &t);
	CHECK("rate_watch_transient_leaves_no_latch", rw.reported == 0);
	CHECK("rate_watch_transient_resets_streak", rw.over_windows == 0);

	/* The dead band must break an ARMED streak, which is what makes the
	 * two-window rule "consecutive" rather than "cumulative".  1.3x is the
	 * shape that matters: the worst benign transient measured on the bench
	 * was 1.43x, i.e. inside this band, so before the fix a single benign
	 * window between two overruns preserved the count and the pair reported
	 * as one sustained episode -- with a duration string computed from the
	 * window count, so two spikes 40 s apart claimed "for 4 s".
	 *
	 * Distinct from the latched hysteresis case below: there the streak is
	 * deliberately held.  The difference is exactly `reported`. */
	memset(&rw, 0, sizeof(rw));
	rw_prime(&rw, &cfg, &t);
	rw_window(&rw, &cfg, 24000, &t);
	CHECK("rate_watch_armed_after_one_over", rw.over_windows == 1);
	rw_window(&rw, &cfg, 5200, &t);
	CHECK("rate_watch_dead_band_breaks_armed_streak", rw.over_windows == 0);
	CHECK("rate_watch_dead_band_did_not_report", rw.reported == 0);
	rw_window(&rw, &cfg, 24000, &t);
	CHECK("rate_watch_non_consecutive_pair_stays_quiet", rw.reported == 0);
	CHECK("rate_watch_non_consecutive_pair_recounts", rw.over_windows == 1);

	/* TWO consecutive over-windows trip it. */
	memset(&rw, 0, sizeof(rw));
	rw_prime(&rw, &cfg, &t);
	rw_window(&rw, &cfg, 24000, &t);
	CHECK("rate_watch_first_window_quiet", rw.reported == 0);
	rw_window(&rw, &cfg, 24000, &t);
	CHECK("rate_watch_second_window_trips", rw.reported == 1);

	/* Latched: no re-report while the overrun persists. */
	rw_window(&rw, &cfg, 24000, &t);
	CHECK("rate_watch_stays_latched", rw.reported == 1);
	CHECK("rate_watch_keeps_counting", rw.over_windows == 3);
	/* One episode, one report.  `reported` stays 1 whether or not the latch
	 * works, so it cannot show this on its own -- a mutant that re-reports
	 * every window passes every other assertion here. */
	CHECK("rate_watch_reports_once_per_episode", rw.reports == 1);

	/* 1.3x sits between the clear and trip thresholds: it must neither trip
	 * nor clear, which is what makes this a hysteresis band rather than one
	 * threshold.  Latched from above, so a clear here would show. */
	rw_window(&rw, &cfg, 5200, &t);
	CHECK("rate_watch_hysteresis_holds_latch", rw.reported == 1);
	CHECK("rate_watch_hysteresis_holds_streak", rw.over_windows == 3);

	/* Back under the clear threshold: releases, and can arm again. */
	rw_window(&rw, &cfg, 4000, &t);
	CHECK("rate_watch_clears", rw.reported == 0);
	CHECK("rate_watch_resets_streak", rw.over_windows == 0);
	rw_window(&rw, &cfg, 24000, &t);
	rw_window(&rw, &cfg, 24000, &t);
	CHECK("rate_watch_rearms", rw.reported == 1);
	CHECK("rate_watch_second_episode_reported", rw.reports == 2);

	/* Zero target disables the check outright.  Without the guard this
	 * divides by zero once a window closes, so the mutant does not merely
	 * fail -- it faults. */
	memset(&rw, 0, sizeof(rw));
	cfg.video0.bitrate = 0;
	rw_prime(&rw, &cfg, &t);
	rw_window(&rw, &cfg, 24000, &t);
	rw_window(&rw, &cfg, 24000, &t);
	CHECK("rate_watch_zero_target_disabled",
		rw.reported == 0 && rw.window_bytes == 0);

	return failures;
}

int test_pipeline_common(void)
{
	int failures = 0;
	SensorSelectConfig cfg;
	SensorSelectResult sensor;
	uint32_t width;
	uint32_t height;
	PipelinePrecropRect rect;
	char resolved[256];
	char tmp_dir[64];
	char fixture_a[160];
	char fixture_b[160];
	int rc;

	failures += test_rate_watch();

	CHECK("pipeline common gop zero", pipeline_common_gop_frames(0.0, 120) == 1);
	CHECK("pipeline common gop fps zero", pipeline_common_gop_frames(1.0, 0) == 30);
	CHECK("pipeline common gop rounded", pipeline_common_gop_frames(1.5, 90) == 135);

	/* compute_precrop: keep_aspect=true, sensor 4:3 → encode 16:9.
	 * 2560x1920 → 2560x1440 with 240px Y offset. */
	rect = pipeline_common_compute_precrop(2560, 1920, 1920, 1080, true);
	CHECK("precrop 4:3->16:9 w", rect.w == 2560);
	CHECK("precrop 4:3->16:9 h", rect.h == 1440);
	CHECK("precrop 4:3->16:9 x", rect.x == 0);
	CHECK("precrop 4:3->16:9 y", rect.y == 240);

	/* compute_precrop: keep_aspect=true, sensor 16:9 → encode 4:3.
	 * 1920x1080 → 1440x1080 with 240px X offset. */
	rect = pipeline_common_compute_precrop(1920, 1080, 1440, 1080, true);
	CHECK("precrop 16:9->4:3 w", rect.w == 1440);
	CHECK("precrop 16:9->4:3 h", rect.h == 1080);
	CHECK("precrop 16:9->4:3 x", rect.x == 240);
	CHECK("precrop 16:9->4:3 y", rect.y == 0);

	/* compute_precrop: keep_aspect=true, matched AR → no crop. */
	rect = pipeline_common_compute_precrop(1920, 1080, 1280, 720, true);
	CHECK("precrop matched-AR w", rect.w == 1920);
	CHECK("precrop matched-AR h", rect.h == 1080);
	CHECK("precrop matched-AR x", rect.x == 0);
	CHECK("precrop matched-AR y", rect.y == 0);

	/* compute_precrop: keep_aspect=false short-circuits to full sensor
	 * regardless of image_w/image_h.  This is what isp.keepAspect=false
	 * gives us. */
	rect = pipeline_common_compute_precrop(2560, 1920, 1920, 1080, false);
	CHECK("precrop !keep 4:3->16:9 w", rect.w == 2560);
	CHECK("precrop !keep 4:3->16:9 h", rect.h == 1920);
	CHECK("precrop !keep 4:3->16:9 x", rect.x == 0);
	CHECK("precrop !keep 4:3->16:9 y", rect.y == 0);

	rect = pipeline_common_compute_precrop(1920, 1080, 1440, 1080, false);
	CHECK("precrop !keep 16:9->4:3 w", rect.w == 1920);
	CHECK("precrop !keep 16:9->4:3 h", rect.h == 1080);
	CHECK("precrop !keep 16:9->4:3 x", rect.x == 0);
	CHECK("precrop !keep 16:9->4:3 y", rect.y == 0);

	/* compute_precrop: 2-pixel alignment of the *cropped* dimension and
	 * its offset.  IMX415-like 3840x2160 down to 4:3 1440x1080 must align
	 * the new width and X offset. */
	rect = pipeline_common_compute_precrop(3840, 2160, 1440, 1080, true);
	CHECK("precrop align cropped w", (rect.w & 1u) == 0);
	CHECK("precrop align cropped x", (rect.x & 1u) == 0);
	CHECK("precrop align kept h", rect.h == 2160);
	CHECK("precrop align kept y", rect.y == 0);

	cfg = pipeline_common_build_sensor_select_config(2, 3, 2688, 1520, 90);
	CHECK("pipeline common cfg pad", cfg.forced_pad == 2);
	CHECK("pipeline common cfg mode", cfg.forced_mode == 3);
	CHECK("pipeline common cfg width", cfg.target_width == 2688);
	CHECK("pipeline common cfg height", cfg.target_height == 1520);
	CHECK("pipeline common cfg fps", cfg.target_fps == 90);

	sensor.mode.minFps = 30;
	sensor.mode.maxFps = 120;
	sensor.fps = 60;
	pipeline_common_report_selected_fps("[test] ", 60, &sensor);
	pipeline_common_report_selected_fps("[test] ", 90, &sensor);
	pipeline_common_report_selected_fps(NULL, 90, NULL);
	CHECK("pipeline common report selected fps", 1);

	width = 3000;
	height = 1600;
	pipeline_common_clamp_image_size("[test] ", 2688, 1520, &width, &height);
	CHECK("pipeline common clamp width", width == 2688);
	CHECK("pipeline common clamp height", height == 1520);

	width = 1920;
	height = 1080;
	pipeline_common_clamp_image_size("[test] ", 2688, 1520, &width, &height);
	CHECK("pipeline common keep width", width == 1920);
	CHECK("pipeline common keep height", height == 1080);

	pipeline_common_clamp_image_size(NULL, 2688, 1520, NULL, &height);
	pipeline_common_clamp_image_size(NULL, 2688, 1520, &width, NULL);
	CHECK("pipeline common clamp null safe", 1);

	/* ── resolve_isp_bin ─────────────────────────────────────────── */
	snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/venc_test_isp_%d", (int)getpid());
	mkdir(tmp_dir, 0700);
	snprintf(fixture_a, sizeof(fixture_a), "%s/configured.bin", tmp_dir);
	snprintf(fixture_b, sizeof(fixture_b), "%s/missing.bin", tmp_dir);
	(void)touch_file(fixture_a);

	/* configured path exists and is readable → use it verbatim */
	resolved[0] = '\0';
	rc = pipeline_common_resolve_isp_bin(fixture_a, "IMX335_MIPI",
		resolved, sizeof(resolved));
	CHECK("isp_bin configured rc", rc == 1);
	CHECK("isp_bin configured path", strcmp(resolved, fixture_a) == 0);

	/* configured but unreadable + sensor name has no fallback installed
	 * (we assume the test host doesn't carry /etc/sensors/imx_test.bin) */
	resolved[0] = 'X';
	rc = pipeline_common_resolve_isp_bin(fixture_b, "imx_test",
		resolved, sizeof(resolved));
	CHECK("isp_bin missing rc", rc == 0);
	CHECK("isp_bin missing empty", resolved[0] == '\0');

	/* empty configured + NULL sensor name → no path */
	resolved[0] = 'X';
	rc = pipeline_common_resolve_isp_bin(NULL, NULL,
		resolved, sizeof(resolved));
	CHECK("isp_bin null sensor rc", rc == 0);
	CHECK("isp_bin null sensor empty", resolved[0] == '\0');

	/* empty configured + sensor name with no alnum prefix → no fallback */
	resolved[0] = 'X';
	rc = pipeline_common_resolve_isp_bin("", "_no_prefix",
		resolved, sizeof(resolved));
	CHECK("isp_bin no-prefix rc", rc == 0);
	CHECK("isp_bin no-prefix empty", resolved[0] == '\0');

	/* NULL out buffer / zero size → safe rc=0 */
	rc = pipeline_common_resolve_isp_bin(fixture_a, "imx335", NULL, 100);
	CHECK("isp_bin null buf rc", rc == 0);
	rc = pipeline_common_resolve_isp_bin(fixture_a, "imx335", resolved, 0);
	CHECK("isp_bin zero size rc", rc == 0);

	/* ── ROI bands ──────────────────────────────────────────────────
	 * The 1280x720 / center 0.3 / steps 1 case is the exact geometry the
	 * Maruko bitstream measurement used, so the numbers here and the
	 * device evidence describe the same rectangle. */
	{
		PipelineRoiBand b;
		int i;

		CHECK("roi band 720p c0.3 s1 rc",
			pipeline_common_roi_band(1280, 720, 0.3f, -30, 1, 0, &b) == 0);
		CHECK("roi band 720p c0.3 s1 x", b.x == 448);
		CHECK("roi band 720p c0.3 s1 w", b.width == 384);
		CHECK("roi band 720p c0.3 s1 y", b.y == 0);
		CHECK("roi band 720p c0.3 s1 h", b.height == 704);
		CHECK("roi band 720p c0.3 s1 qp", b.qp == -30);

		/* Two steps: outer band is wider and weaker, inner takes the full
		 * delta.  A backend where a higher index overrides a lower one in
		 * the overlap therefore lands full strength in the centre. */
		CHECK("roi band s2 outer rc",
			pipeline_common_roi_band(1280, 720, 0.4f, -30, 2, 0, &b) == 0);
		CHECK("roi band s2 outer w", b.width == 896);
		CHECK("roi band s2 outer x", b.x == 192);
		CHECK("roi band s2 outer qp", b.qp == -15);
		CHECK("roi band s2 inner rc",
			pipeline_common_roi_band(1280, 720, 0.4f, -30, 2, 1, &b) == 0);
		CHECK("roi band s2 inner w", b.width == 512);
		CHECK("roi band s2 inner x", b.x == 384);
		CHECK("roi band s2 inner qp", b.qp == -30);

		/* A positive delta keeps its sign through the taper — the centre
		 * gets SOFTER, which is the arm the device A/B inverted on. */
		CHECK("roi band positive taper rc",
			pipeline_common_roi_band(1280, 720, 0.4f, 30, 2, 0, &b) == 0);
		CHECK("roi band positive taper qp", b.qp == 15);

		/* Every edge 32-px aligned, at every step count, or the SDK
		 * refuses the rect (H.265 CTU constraint). */
		for (i = 0; i < 4; i++) {
			if (pipeline_common_roi_band(1920, 1080, 0.35f, -12,
				4, i, &b) != 0)
				continue;
			CHECK("roi band aligned x", b.x % 32 == 0);
			CHECK("roi band aligned w", b.width % 32 == 0);
			CHECK("roi band aligned h", b.height % 32 == 0);
			CHECK("roi band inside frame", b.x + b.width <= 1920);
		}

		/* Skips, not silent zero-size regions: a rect that rounds away
		 * must be dropped rather than programmed, or the encoder gets an
		 * enabled region of zero area. */
		CHECK("roi band index negative",
			pipeline_common_roi_band(1280, 720, 0.3f, -30, 1, -1, &b) != 0);
		CHECK("roi band index past steps",
			pipeline_common_roi_band(1280, 720, 0.3f, -30, 2, 2, &b) != 0);
		CHECK("roi band null out",
			pipeline_common_roi_band(1280, 720, 0.3f, -30, 1, 0, NULL) != 0);
		CHECK("roi band width rounds away",
			pipeline_common_roi_band(16, 720, 0.3f, -30, 1, 0, &b) != 0);
		CHECK("roi band height rounds away",
			pipeline_common_roi_band(1280, 16, 0.3f, -30, 1, 0, &b) != 0);

		/* Out-of-domain center_frac is clamped, not trusted.  Before the
		 * clamp both of these returned SUCCESS with a garbage rect:
		 * frac>1 underflowed the unsigned (width-rw)/2 to ~2^31, and a
		 * negative frac is an out-of-range float-to-unsigned conversion
		 * (undefined behaviour) that produced a ~4 GB width.  Asserted
		 * as "identical to the clamped value" rather than "not huge", so
		 * the test pins the behaviour and not just the absence of one
		 * symptom. */
		{
			PipelineRoiBand hi, lo, ref_hi, ref_lo;

			CHECK("roi band frac>1 rc",
				pipeline_common_roi_band(1280, 720, 1.5f, -30,
					1, 0, &hi) == 0);
			CHECK("roi band frac 0.9 rc",
				pipeline_common_roi_band(1280, 720, 0.9f, -30,
					1, 0, &ref_hi) == 0);
			CHECK("roi band frac>1 clamps x", hi.x == ref_hi.x);
			CHECK("roi band frac>1 clamps w",
				hi.width == ref_hi.width);
			CHECK("roi band frac>1 inside frame",
				hi.x + hi.width <= 1280);

			CHECK("roi band frac<0 rc",
				pipeline_common_roi_band(1280, 720, -2.0f, -30,
					1, 0, &lo) == 0);
			CHECK("roi band frac 0.1 rc",
				pipeline_common_roi_band(1280, 720, 0.1f, -30,
					1, 0, &ref_lo) == 0);
			CHECK("roi band frac<0 clamps x", lo.x == ref_lo.x);
			CHECK("roi band frac<0 clamps w",
				lo.width == ref_lo.width);
			CHECK("roi band frac<0 inside frame",
				lo.x + lo.width <= 1280);
		}
	}

	/* cleanup */
	unlink(fixture_a);
	rmdir(tmp_dir);

	return failures;
}
