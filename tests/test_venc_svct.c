/*
 * Unit tests for venc_svct.h — the shared SVC-T frame classifier used by
 * both backends for TRAIL_N rewriting and per-layer output routing.
 */

#include "venc_svct.h"
#include "test_helpers.h"

static int test_classifier_truth_table(void)
{
	int failures = 0;

	/* With refPred active (ref_base > 0): base labels are never
	 * enhancement, both ENHANCE_* labels are. */
	CHECK("enh_base_idr",
		!venc_svct_frame_is_enhance(1, VENC_SVCT_REFTYPE_BASE_IDR));
	CHECK("enh_base_p_refbybase",
		!venc_svct_frame_is_enhance(1,
			VENC_SVCT_REFTYPE_BASE_P_REFBYBASE));
	CHECK("enh_base_p_refbyenhance",
		!venc_svct_frame_is_enhance(1,
			VENC_SVCT_REFTYPE_BASE_P_REFBYENHANCE));
	CHECK("enh_enhance_refbyenhance",
		venc_svct_frame_is_enhance(1,
			VENC_SVCT_REFTYPE_ENHANCE_P_REFBYENHANCE));
	CHECK("enh_enhance_notforref",
		venc_svct_frame_is_enhance(1,
			VENC_SVCT_REFTYPE_ENHANCE_P_NOTFORREF));

	/* notforref is the strict subset safe for TRAIL_N rewriting. */
	CHECK("nfr_only_notforref",
		venc_svct_frame_is_notforref(1,
			VENC_SVCT_REFTYPE_ENHANCE_P_NOTFORREF));
	CHECK("nfr_not_refbyenhance",
		!venc_svct_frame_is_notforref(1,
			VENC_SVCT_REFTYPE_ENHANCE_P_REFBYENHANCE));
	CHECK("nfr_not_base",
		!venc_svct_frame_is_notforref(1, VENC_SVCT_REFTYPE_BASE_IDR));

	/* ref_base == 0 gates everything off — flat single-ref stream,
	 * every frame matters regardless of the label the SDK reports. */
	CHECK("gate_enh_off",
		!venc_svct_frame_is_enhance(0,
			VENC_SVCT_REFTYPE_ENHANCE_P_NOTFORREF));
	CHECK("gate_nfr_off",
		!venc_svct_frame_is_notforref(0,
			VENC_SVCT_REFTYPE_ENHANCE_P_NOTFORREF));

	/* Out-of-range labels (future SDK values) never classify. */
	CHECK("unknown_label_not_enh", !venc_svct_frame_is_enhance(1, 250));

	return failures;
}

int test_venc_svct(void)
{
	int failures = 0;

	failures += test_classifier_truth_table();

	return failures;
}
