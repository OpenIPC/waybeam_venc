#include "intra_refresh.h"
#include "test_helpers.h"

#include <math.h>
#include <string.h>

static int approx(double a, double b)
{
	return fabs(a - b) < 1e-6;
}

int test_intra_refresh(void)
{
	int failures = 0;
	IntraRefreshDerived ir;

	/* ── parse / name round-trip ─────────────────────────────────── */
	CHECK("parse_off",      intra_refresh_parse_mode("off")      == INTRA_MODE_OFF);
	CHECK("parse_fast",     intra_refresh_parse_mode("fast")     == INTRA_MODE_FAST);
	CHECK("parse_balanced", intra_refresh_parse_mode("balanced") == INTRA_MODE_BALANCED);
	CHECK("parse_robust",   intra_refresh_parse_mode("robust")   == INTRA_MODE_ROBUST);
	CHECK("parse_caseinsensitive",
		intra_refresh_parse_mode("BALANCED") == INTRA_MODE_BALANCED);
	CHECK("parse_unknown_off",
		intra_refresh_parse_mode("bogus") == INTRA_MODE_OFF);
	CHECK("parse_null_off",
		intra_refresh_parse_mode(NULL) == INTRA_MODE_OFF);
	CHECK("parse_empty_off",
		intra_refresh_parse_mode("") == INTRA_MODE_OFF);

	CHECK("name_off",      strcmp(intra_refresh_mode_name(INTRA_MODE_OFF),      "off")      == 0);
	CHECK("name_fast",     strcmp(intra_refresh_mode_name(INTRA_MODE_FAST),     "fast")     == 0);
	CHECK("name_balanced", strcmp(intra_refresh_mode_name(INTRA_MODE_BALANCED), "balanced") == 0);
	CHECK("name_robust",   strcmp(intra_refresh_mode_name(INTRA_MODE_ROBUST),   "robust")   == 0);

	/* ── OFF mode: all zeros ─────────────────────────────────────── */
	intra_refresh_compute(INTRA_MODE_OFF, 1080, 60, 1, 0, 0, 0.0, &ir);
	CHECK("off_mode_zero", ir.mode == INTRA_MODE_OFF && ir.lines == 0 &&
		ir.gop_frames == 0 && ir.req_iqp == 0 && ir.target_ms == 0);

	/* ── 1080p H.265: total_rows = ceil(1080/32) = 34 ───────────── */
	intra_refresh_compute(INTRA_MODE_FAST, 1080, 60, 1, 0, 0, 0.0, &ir);
	CHECK("1080p60_h265_fast_total_rows", ir.total_rows == 34);
	/* refresh_frames = round(60 * 150 / 1000) = 9; lines = ceil(34/9) = 4 */
	CHECK("1080p60_h265_fast_lines",      ir.lines == 4);
	/* gop_frames = ceil(34/4) = 9 → 9/60 = 0.15s */
	CHECK("1080p60_h265_fast_gop_frames", ir.gop_frames == 9);
	CHECK("1080p60_h265_fast_gop_sec",    approx(ir.gop_sec, 9.0/60.0));
	CHECK("1080p60_h265_fast_qp_default", ir.req_iqp == 36);
	CHECK("1080p60_h265_fast_target_ms",  ir.target_ms == 150);
	CHECK("1080p60_h265_fast_no_clamp",   ir.lines_clamped == 0);
	CHECK("1080p60_h265_fast_gop_auto",   ir.gop_overridden == 0);

	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 1, 0, 0, 0.0, &ir);
	/* refresh_frames = round(60 * 500 / 1000) = 30; lines = ceil(34/30) = 2 */
	CHECK("1080p60_h265_balanced_lines",      ir.lines == 2);
	/* gop_frames = ceil(34/2) = 17 → 17/60 ≈ 0.283s */
	CHECK("1080p60_h265_balanced_gop_frames", ir.gop_frames == 17);

	intra_refresh_compute(INTRA_MODE_ROBUST, 1080, 60, 1, 0, 0, 0.0, &ir);
	/* refresh_frames = round(60 * 1000 / 1000) = 60; lines = ceil(34/60) = 1 */
	CHECK("1080p60_h265_robust_lines",      ir.lines == 1);
	/* gop_frames = ceil(34/1) = 34 → 34/60 ≈ 0.567s */
	CHECK("1080p60_h265_robust_gop_frames", ir.gop_frames == 34);

	/* ── 720p H.265: total_rows = ceil(720/32) = 23 ─────────────── */
	intra_refresh_compute(INTRA_MODE_FAST, 720, 60, 1, 0, 0, 0.0, &ir);
	CHECK("720p60_h265_fast_total_rows", ir.total_rows == 23);
	CHECK("720p60_h265_fast_lines", ir.lines == 3); /* ceil(23/9) = 3 */

	/* ── H.264 default QP = 45, lcu_h = 16 ──────────────────────── */
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 30, 0, 0, 0, 0.0, &ir);
	CHECK("1080p30_h264_total_rows", ir.total_rows == 68); /* ceil(1080/16) */
	CHECK("1080p30_h264_qp_default", ir.req_iqp == 29);

	/* ── Override: explicit lines wins, auto-GOP recomputes ─────── */
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 1, 4, 0, 0.0, &ir);
	CHECK("override_lines_4",       ir.lines == 4);
	CHECK("override_lines_gop_frames", ir.gop_frames == 9); /* ceil(34/4) */
	CHECK("override_lines_no_clamp", ir.lines_clamped == 0);

	/* ── Override: explicit lines clamped to total_rows ─────────── */
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 1, 999, 0, 0.0, &ir);
	CHECK("override_lines_clamped_value",   ir.lines == 34);
	CHECK("override_lines_clamped_flag",    ir.lines_clamped == 1);
	CHECK("override_lines_clamped_gop",     ir.gop_frames == 1);

	/* ── Override: explicit QP wins over codec default ──────────── */
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 1, 0, 30, 0.0, &ir);
	CHECK("override_qp",        ir.req_iqp == 30);

	/* ── Override: explicit gop_size suppresses auto-GOP ────────── */
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 1, 0, 0, 2.0, &ir);
	CHECK("override_gop_flag",       ir.gop_overridden == 1);
	CHECK("override_gop_zero",       ir.gop_frames == 0);
	CHECK("override_gop_lines_kept", ir.lines == 2);
	CHECK("override_gop_qp_kept",    ir.req_iqp == 32);  /* balanced default */

	/* ── Per-mode QP defaults (codec-aware) ─────────────────────── */
	intra_refresh_compute(INTRA_MODE_FAST,     1080, 60, 1, 0, 0, 0.0, &ir);
	CHECK("h265_fast_qp",     ir.req_iqp == 36);
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 1, 0, 0, 0.0, &ir);
	CHECK("h265_balanced_qp", ir.req_iqp == 32);
	intra_refresh_compute(INTRA_MODE_ROBUST,   1080, 60, 1, 0, 0, 0.0, &ir);
	CHECK("h265_robust_qp",   ir.req_iqp == 28);
	intra_refresh_compute(INTRA_MODE_FAST,     1080, 60, 0, 0, 0, 0.0, &ir);
	CHECK("h264_fast_qp",     ir.req_iqp == 33);
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 0, 0, 0, 0.0, &ir);
	CHECK("h264_balanced_qp", ir.req_iqp == 29);
	intra_refresh_compute(INTRA_MODE_ROBUST,   1080, 60, 0, 0, 0, 0.0, &ir);
	CHECK("h264_robust_qp",   ir.req_iqp == 25);

	/* ── Edge: zero height/fps treated as off ───────────────────── */
	intra_refresh_compute(INTRA_MODE_BALANCED, 0, 60, 1, 0, 0, 0.0, &ir);
	CHECK("zero_height_off", ir.mode == INTRA_MODE_OFF);
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 0, 1, 0, 0, 0.0, &ir);
	CHECK("zero_fps_off", ir.mode == INTRA_MODE_OFF);

	/* ── Null out is no-op (does not crash) ─────────────────────── */
	intra_refresh_compute(INTRA_MODE_BALANCED, 1080, 60, 1, 0, 0, 0.0, NULL);
	CHECK("null_out_no_crash", 1);

	return failures;
}
