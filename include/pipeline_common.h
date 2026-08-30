#ifndef PIPELINE_COMMON_H
#define PIPELINE_COMMON_H

#ifndef PLATFORM_CV610
#include "sensor_select.h"
#endif

#include "venc_config.h"

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
 *                       image_w x image_h with 2-pixel alignment.
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

/** Rolling delivered-vs-target bitrate check.
 *
 * The encoder can lose rate control entirely and there is no in-band signal
 * for it on two of the three backends: the frame QP that shows the failure is
 * CV610-only (see rtp_sidecar.h -- SigmaStar fills only refType).  What every
 * backend does have is the frame size, and the failure is far easier to see in
 * the consequence than in the cause.
 *
 * The mechanism worth catching: fpv.roiQp is a RELATIVE delta, so CBR pays for
 * the ROI discount by raising the frame's base QP roughly 1:1.  Once
 * base_qp + |roiQp| passes the controller's QP ceiling the base pins there and
 * the target is missed by multiples -- measured on a CV610 bench at 5.8x with
 * video0.max_qp 40 and 12.0x at 35, both at a roiQp of only -20.
 *
 * Thresholds are compiled in, not configurable, because the measurement leaves
 * no room to tune: normal operation ran 0.96-1.06x of target across every arm,
 * the worst benign scene transient hit 1.43x for a single window, and a real
 * collapse ran 1.9x-39x and did not decay.  Zero bitrate disables the check. */
typedef struct {
	uint64_t window_start_us;
	uint64_t window_bytes;
	uint8_t  over_windows;
	uint8_t  reported;    /* latched, so one episode logs once */
	uint16_t reports;     /* episodes actually reported; the latch is not
	                       * observable from `reported` alone, since that
	                       * stays 1 either way */
} PipelineRateWatch;

/** Call once per encoded frame, with the configured (not measured) target. */
void pipeline_common_rate_watch(PipelineRateWatch *rw, const VencConfig *cfg,
	uint32_t frame_bytes, uint64_t now_us);

/** Maximum number of ROI band regions. */
#define PIPELINE_ROI_MAX_STEPS 4

/** One horizontal ROI band, in encoder pixel coordinates.
 *
 * Horizontal bands, delta QP tapering from the innermost band (full qp)
 * outward.  Higher index = narrower = stronger, so on a backend where a
 * higher-index region overrides a lower one in the overlap the centre lands the
 * full delta and the edges the weakest step.
 *
 * "Full-height, centred" is the intent, not the arithmetic: every edge is
 * rounded DOWN to a 32-px multiple for H.265 CTU compatibility, so a 1080-row
 * frame yields height 1056 and leaves the bottom 24 rows outside every band,
 * and an origin that rounds down can sit up to 31 px left of true centre
 * (1920 wide, centre 0.35, index 3 -> x=608, w=672, band centre 944 vs 960).
 *
 * Shared because all three backends draw the identical rectangle; only the SDK
 * struct it is copied into differs (MI_VENC_RoiCfg_t on the two SigmaStar
 * parts, ot_venc_roi_attr on CV610). */
typedef struct {
	uint32_t x, y, width, height;
	int qp;
} PipelineRoiBand;

/** Compute band `index` of `steps`.  Returns 0 and fills *out when the band is
 * usable, -1 when the caller should skip it: a NULL out, an index outside
 * [0, steps), or a rectangle that degenerates to zero width or height once
 * aligned (a frame under 32 px, or a centre fraction that rounds away).
 *
 * center_frac is CLAMPED to [0.1, 0.9] here, not merely assumed.  All three
 * backends clamp before calling and config load validates the field, so an
 * out-of-range value is unreachable today -- but this is now a cross-TU
 * primitive with three callers, and the failure mode without the clamp is not a
 * bad number: frac > 1 underflows the unsigned (width - rw)/2 to ~2^31, and a
 * negative frac makes (uint32_t)(frac * width) an out-of-range float-to-unsigned
 * conversion, i.e. undefined behaviour.  Both returned SUCCESS with a garbage
 * rect.  A shared primitive should be total over its declared domain. */
int pipeline_common_roi_band(uint32_t width, uint32_t height,
	float center_frac, int qp, int steps, int index,
	PipelineRoiBand *out);

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
