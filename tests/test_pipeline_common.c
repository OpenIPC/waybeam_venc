#include "pipeline_common.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

	/* cleanup */
	unlink(fixture_a);
	rmdir(tmp_dir);

	return failures;
}
