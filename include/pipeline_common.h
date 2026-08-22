#ifndef PIPELINE_COMMON_H
#define PIPELINE_COMMON_H

#ifndef PLATFORM_CV610
#include "sensor_select.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/** Convert GOP duration in seconds to frame count. */
uint32_t pipeline_common_gop_frames(double gop_seconds, uint32_t fps);

/** Build sensor selection config from requested video parameters. */
#ifndef PLATFORM_CV610
SensorSelectConfig pipeline_common_build_sensor_select_config(int forced_pad,
	int forced_mode, uint32_t target_width, uint32_t target_height,
	uint32_t target_fps);
/** Log the selected sensor mode and actual frame rate. */
void pipeline_common_report_selected_fps(const char *prefix,
	uint32_t requested_fps, const SensorSelectResult *sensor);
#endif
/** Clamp requested image dimensions to maximum supported size. */
void pipeline_common_clamp_image_size(const char *prefix, uint32_t max_width,
	uint32_t max_height, uint32_t *image_width, uint32_t *image_height);

/** Precrop rectangle for aspect ratio correction. */
typedef struct {
	uint16_t x, y, w, h;
} PipelinePrecropRect;

/** Cap AE max shutter time for target FPS.
 * shutter_rule_180 = true → cap to 1/(2*fps) (180° shutter rule).
 * shutter_rule_180 = false → cap to 1/fps (frame-period cap).
 * fps = 0 is a no-op. */
int pipeline_common_cap_exposure_for_fps(uint32_t fps,
	bool shutter_rule_180);

/** Compute crop rectangle for the VIF/SCL stage.
 * keep_aspect = true  → center-crop the sensor to the aspect ratio of
 *                       image_w x image_h.  Sizes and offsets are floored
 *                       to 16 px (VIF/VPE silently produce nothing for an
 *                       unaligned window), so the crop aspect may differ
 *                       from the target by up to 16/size.
 * keep_aspect = false → return the full sensor area; image_w/image_h are
 *                       ignored and the downstream scaler will stretch. */
PipelinePrecropRect pipeline_common_compute_precrop(uint32_t sensor_w,
	uint32_t sensor_h, uint32_t image_w, uint32_t image_h,
	bool keep_aspect);

/** Resolve which ISP bin path to load.  Resolution order:
 *   1. configured_path is non-empty AND access(R_OK) succeeds → use it
 *   2. else /etc/sensors/<lowercase prefix of sensor_name>.bin if readable
 *      (prefix = sensor_name truncated at first non-alnum, e.g.
 *      "IMX335_MIPI" → "imx335")
 *   3. else write "" (caller skips the load)
 *
 * Logs the chosen path and the reason on stdout (one line, "> ISP bin: ...").
 *
 * out_path must hold at least 256 bytes.  sensor_name may be NULL or
 * empty — the fallback is then skipped.
 *
 * Returns 1 if a path was chosen (out_path filled), 0 if no path
 * could be resolved (out_path = ""). */
int pipeline_common_resolve_isp_bin(const char *configured_path,
	const char *sensor_name, char *out_path, size_t out_sz);

/** Scale QP proportionally for a given band level.
 * level 1 = outermost (weakest), level steps = innermost (full qp).
 * Higher-index ROI regions override lower ones in overlap zones. */
static inline int pipeline_common_scale_roi_qp(int qp, int level, int steps)
{
	int mag;

	if (steps < 1 || level < 1 || level > steps)
		return 0;
	mag = abs(qp);
	mag = (mag * level + steps / 2) / steps;
	return qp < 0 ? -mag : mag;
}

/** Maximum number of ROI band regions. */
#define PIPELINE_ROI_MAX_STEPS 4

/** Upper bound the live-apply path accepts for video0.fps.
 *
 * A live `fps` request above the current sensor mode's max is intentionally
 * NOT rejected: the per-platform apply_fps() clamps it down to the mode's
 * sensor_fps for the actual VPE->VENC rebind, while the requested value is
 * still committed to the config so a subsequent respawn/sensor_select can
 * pick a higher-fps mode for it (sensor_mode_clamp_fps() re-clamps there too).
 * This lets a client pre-stage e.g. fps=144 while parked in a 100fps mode so
 * the auto sensor-select lands on the 144fps mode on the next mode switch,
 * instead of the mode being clamped down before it is ever entered.
 *
 * 144 is the ceiling because it is the highest fps any current mode offers;
 * anything above it is a client error and still rejected. */
#define PIPELINE_LIVE_FPS_MAX 144u

#endif /* PIPELINE_COMMON_H */
