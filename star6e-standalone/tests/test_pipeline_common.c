#include "pipeline_common.h"
#include "test_helpers.h"

int test_pipeline_common(void)
{
	int failures = 0;
	SensorSelectConfig cfg;
	SensorSelectResult sensor;
	uint32_t width;
	uint32_t height;

	CHECK("pipeline common gop zero", pipeline_common_gop_frames(0.0, 120) == 1);
	CHECK("pipeline common gop fps zero", pipeline_common_gop_frames(1.0, 0) == 30);
	CHECK("pipeline common gop rounded", pipeline_common_gop_frames(1.5, 90) == 135);

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
