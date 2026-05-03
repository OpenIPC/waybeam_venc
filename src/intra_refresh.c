#include "intra_refresh.h"

#include <string.h>
#include <strings.h>

static uint32_t mode_target_ms(IntraRefreshMode m)
{
	switch (m) {
	case INTRA_MODE_FAST:     return 150;
	case INTRA_MODE_BALANCED: return 500;
	case INTRA_MODE_ROBUST:   return 1000;
	case INTRA_MODE_OFF:
	default:                  return 0;
	}
}

/* Per-mode stripe QP. H.264 runs ~3 lower than H.265 for equivalent
 * perceived quality (PSNR/SSIM differ across the codecs at the same QP). */
static uint32_t mode_default_qp(IntraRefreshMode m, int is_h265)
{
	switch (m) {
	case INTRA_MODE_FAST:     return is_h265 ? 36u : 33u;
	case INTRA_MODE_BALANCED: return is_h265 ? 32u : 29u;
	case INTRA_MODE_ROBUST:   return is_h265 ? 28u : 25u;
	case INTRA_MODE_OFF:
	default:                  return 0;
	}
}

IntraRefreshMode intra_refresh_parse_mode(const char *s)
{
	if (!s || !*s) return INTRA_MODE_OFF;
	if (strcasecmp(s, "off")      == 0) return INTRA_MODE_OFF;
	if (strcasecmp(s, "fast")     == 0) return INTRA_MODE_FAST;
	if (strcasecmp(s, "balanced") == 0) return INTRA_MODE_BALANCED;
	if (strcasecmp(s, "robust")   == 0) return INTRA_MODE_ROBUST;
	return INTRA_MODE_OFF;
}

const char *intra_refresh_mode_name(IntraRefreshMode m)
{
	switch (m) {
	case INTRA_MODE_FAST:     return "fast";
	case INTRA_MODE_BALANCED: return "balanced";
	case INTRA_MODE_ROBUST:   return "robust";
	case INTRA_MODE_OFF:
	default:                  return "off";
	}
}

void intra_refresh_compute(
	IntraRefreshMode mode,
	uint32_t height, uint32_t fps, int is_h265,
	uint32_t override_lines,
	uint32_t override_qp,
	double   explicit_gop_sec,
	IntraRefreshDerived *out)
{
	if (!out) return;
	memset(out, 0, sizeof(*out));
	out->mode = mode;

	if (mode == INTRA_MODE_OFF || height == 0 || fps == 0) {
		out->mode = INTRA_MODE_OFF;
		return;
	}

	uint32_t lcu_h        = is_h265 ? 32u : 16u;
	uint32_t total_rows   = (height + lcu_h - 1u) / lcu_h;
	if (total_rows == 0) total_rows = 1;
	uint32_t target_ms    = mode_target_ms(mode);

	/* refresh_frames = round(fps * target_ms / 1000), at least 1. */
	uint32_t refresh_frames = (fps * target_ms + 500u) / 1000u;
	if (refresh_frames < 1u) refresh_frames = 1u;

	uint32_t auto_lines = (total_rows + refresh_frames - 1u) / refresh_frames;
	if (auto_lines < 1u) auto_lines = 1u;
	if (auto_lines > total_rows) auto_lines = total_rows;

	uint32_t effective_lines;
	if (override_lines > 0) {
		if (override_lines > total_rows) {
			effective_lines = total_rows;
			out->lines_clamped = 1;
		} else {
			effective_lines = override_lines;
		}
	} else {
		effective_lines = auto_lines;
	}

	/* Auto GOP: one IDR per full GDR pass, derived from effective lines.
	 * Suppressed when caller supplied an explicit gop_size > 0. */
	uint32_t gop_frames = 0;
	double   gop_sec    = 0.0;
	if (explicit_gop_sec > 0.0) {
		out->gop_overridden = 1;
	} else {
		gop_frames = (total_rows + effective_lines - 1u) / effective_lines;
		if (gop_frames < 1u) gop_frames = 1u;
		gop_sec    = (double)gop_frames / (double)fps;
	}

	uint32_t req_iqp = override_qp > 0
		? override_qp
		: mode_default_qp(mode, is_h265);

	out->target_ms  = target_ms;
	out->total_rows = total_rows;
	out->lines      = effective_lines;
	out->gop_frames = gop_frames;
	out->gop_sec    = gop_sec;
	out->req_iqp    = req_iqp;
}
