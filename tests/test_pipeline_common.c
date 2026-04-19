#include "pipeline_common.h"
#include "test_helpers.h"

int test_pipeline_common(void)
{
	int failures = 0;
	SensorSelectConfig cfg;
	SensorSelectResult sensor;
	uint32_t width;
	uint32_t height;
	PipelinePrecropRect rect;

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

	return failures;
}
