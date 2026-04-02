#include "gop_controller.h"
#include "test_helpers.h"

#include <string.h>

static GopConfig test_config(void)
{
	GopConfig cfg;

	gop_config_defaults(&cfg);
	cfg.enable_variable_gop = 1;
	cfg.max_gop_length = 60;
	cfg.min_gop_length = 10;
	cfg.defer_timeout_frames = 20;
	cfg.scene_change_threshold = 250;
	cfg.scene_change_holdoff = 2;
	cfg.idr_qp_boost = 4;
	return cfg;
}

static SceneEstimate no_change(void)
{
	SceneEstimate sc;

	memset(&sc, 0, sizeof(sc));
	sc.complexity = 128;
	sc.scene_change = 0;
	sc.budget_ratio = 1000;
	return sc;
}

static SceneEstimate with_change(void)
{
	SceneEstimate sc;

	memset(&sc, 0, sizeof(sc));
	sc.complexity = 230;
	sc.scene_change = 1;
	sc.budget_ratio = 2500;
	return sc;
}

static int test_gop_forced_idr(void)
{
	GopControllerState gc;
	GopConfig cfg = test_config();
	SceneEstimate sc = no_change();
	uint8_t boost;
	int failures = 0;
	int idr;
	uint16_t i;

	gop_ctrl_init(&gc, &cfg);

	/* Run through max_gop_length frames — should force IDR at 60 */
	for (i = 0; i < 59; i++) {
		idr = gop_ctrl_on_frame(&gc, &sc, &boost);
		CHECK("no idr before max", idr == 0);
	}

	idr = gop_ctrl_on_frame(&gc, &sc, &boost);
	CHECK("forced idr at max", idr == 1);
	CHECK("forced idr boost", boost == 4);

	/* Simulate IDR encoded */
	gop_ctrl_idr_encoded(&gc);

	{
		GopState state;
		uint16_t fsi;
		gop_ctrl_get_state(&gc, &state, &fsi);
		CHECK("state after idr", state == GOP_STATE_NORMAL);
		CHECK("fsi after idr", fsi == 0);
	}

	return failures;
}

static int test_gop_scene_change(void)
{
	GopControllerState gc;
	GopConfig cfg = test_config();
	SceneEstimate sc = no_change();
	SceneEstimate sc_change = with_change();
	uint8_t boost;
	int failures = 0;
	int idr;
	uint16_t i;

	gop_ctrl_init(&gc, &cfg);

	/* Run past min_gop_length */
	for (i = 0; i < 15; i++) {
		gop_ctrl_on_frame(&gc, &sc, &boost);
	}

	/* Scene change should trigger IDR */
	idr = gop_ctrl_on_frame(&gc, &sc_change, &boost);
	CHECK("scene change idr", idr == 1);
	CHECK("scene change stats", gc.scene_change_idrs == 1);

	return failures;
}

static int test_gop_min_gop_respected(void)
{
	GopControllerState gc;
	GopConfig cfg = test_config();
	SceneEstimate sc_change = with_change();
	uint8_t boost;
	int failures = 0;
	int idr;
	uint16_t i;

	gop_ctrl_init(&gc, &cfg);

	/* Scene change within min_gop_length — should NOT trigger */
	for (i = 0; i < 5; i++) {
		idr = gop_ctrl_on_frame(&gc, &sc_change, &boost);
		CHECK("no idr within min gop", idr == 0);
	}

	return failures;
}

static int test_gop_external_request(void)
{
	GopControllerState gc;
	GopConfig cfg = test_config();
	SceneEstimate sc = no_change();
	uint8_t boost;
	int failures = 0;
	int idr;
	uint16_t i;

	gop_ctrl_init(&gc, &cfg);

	/* Get past min_gop_length */
	for (i = 0; i < 12; i++) {
		gop_ctrl_on_frame(&gc, &sc, &boost);
	}

	/* External IDR request */
	gop_ctrl_request_idr(&gc, 6);

	idr = gop_ctrl_on_frame(&gc, &sc, &boost);
	CHECK("external idr", idr == 1);
	CHECK("external boost", boost == 6);

	return failures;
}

static int test_gop_defer(void)
{
	GopControllerState gc;
	GopConfig cfg = test_config();
	SceneEstimate sc = no_change();
	SceneEstimate sc_change = with_change();
	uint8_t boost;
	int failures = 0;
	int idr;
	uint16_t i;

	gop_ctrl_init(&gc, &cfg);

	/* Get past min_gop_length */
	for (i = 0; i < 12; i++) {
		gop_ctrl_on_frame(&gc, &sc, &boost);
	}

	/* Trigger scene change → IDR_READY */
	idr = gop_ctrl_on_frame(&gc, &sc_change, &boost);
	CHECK("scene triggers idr ready", idr == 1);

	/* The IDR was returned but hasn't been encoded yet.
	 * Reset to IDR_READY state for defer test. */
	gc.state = GOP_STATE_IDR_READY;

	/* Defer it */
	gop_ctrl_defer_idr(&gc);
	{
		GopState state;
		gop_ctrl_get_state(&gc, &state, NULL);
		CHECK("defer state", state == GOP_STATE_IDR_DEFERRED);
	}

	/* Run in deferred state — should not insert IDR */
	for (i = 0; i < 5; i++) {
		idr = gop_ctrl_on_frame(&gc, &sc, &boost);
		CHECK("no idr while deferred", idr == 0);
	}

	/* Clear defer — should trigger IDR on next frame */
	gop_ctrl_clear_defer(&gc);
	idr = gop_ctrl_on_frame(&gc, &sc, &boost);
	CHECK("idr after clear defer", idr == 1);

	return failures;
}

static int test_gop_defer_timeout(void)
{
	GopControllerState gc;
	GopConfig cfg = test_config();
	SceneEstimate sc = no_change();
	SceneEstimate sc_change = with_change();
	uint8_t boost;
	int failures = 0;
	int idr;
	uint16_t i;

	gop_ctrl_init(&gc, &cfg);

	/* Get past min_gop_length and trigger scene change */
	for (i = 0; i < 12; i++) {
		gop_ctrl_on_frame(&gc, &sc, &boost);
	}

	gop_ctrl_on_frame(&gc, &sc_change, &boost);
	gc.state = GOP_STATE_IDR_READY;

	/* Defer it */
	gop_ctrl_defer_idr(&gc);

	/* Run through defer_timeout_frames */
	idr = 0;
	for (i = 0; i < 25; i++) {
		idr = gop_ctrl_on_frame(&gc, &sc, &boost);
		if (idr)
			break;
	}

	CHECK("defer timeout forces idr", idr == 1);

	return failures;
}

static int test_gop_passthrough(void)
{
	GopControllerState gc;
	GopConfig cfg = test_config();
	SceneEstimate sc_change = with_change();
	uint8_t boost;
	int failures = 0;
	int idr;
	uint16_t i;

	cfg.enable_variable_gop = 0;  /* passthrough */
	gop_ctrl_init(&gc, &cfg);

	/* Even with scene changes and past max, should never trigger */
	for (i = 0; i < 100; i++) {
		idr = gop_ctrl_on_frame(&gc, &sc_change, &boost);
		CHECK("passthrough no idr", idr == 0);
	}

	/* But frames_since_idr should still count */
	{
		uint16_t fsi;
		gop_ctrl_get_state(&gc, NULL, &fsi);
		CHECK("passthrough fsi counts", fsi == 100);
	}

	return failures;
}

int test_gop_controller(void)
{
	int failures = 0;

	failures += test_gop_forced_idr();
	failures += test_gop_scene_change();
	failures += test_gop_min_gop_respected();
	failures += test_gop_external_request();
	failures += test_gop_defer();
	failures += test_gop_defer_timeout();
	failures += test_gop_passthrough();

	return failures;
}
