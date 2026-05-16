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

/* Per-mode stripe QP for H.265. */
static uint32_t mode_default_qp(IntraRefreshMode m)
{
	switch (m) {
	case INTRA_MODE_FAST:     return 36u;
	case INTRA_MODE_BALANCED: return 32u;
	case INTRA_MODE_ROBUST:   return 28u;
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
	uint32_t height, uint32_t fps,
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

	/* H.265 CTU is 32×32. */
	uint32_t total_rows   = (height + 31u) / 32u;
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
		: mode_default_qp(mode);

	out->target_ms  = target_ms;
	out->total_rows = total_rows;
	out->lines      = effective_lines;
	out->gop_frames = gop_frames;
	out->gop_sec    = gop_sec;
	out->req_iqp    = req_iqp;
}
