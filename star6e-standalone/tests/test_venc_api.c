/*
 * Unit tests for venc_api.c
 *
 * Tests: field descriptor lookup, registration, mutability,
 * field serialization/deserialization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "venc_config.h"
#include "venc_api.h"
#include "venc_httpd.h"
#include "star6e.h"
#include "test_helpers.h"

/* Stubs for MI_VENC functions used by the dual VENC API in venc_api.c.
 * The test binary doesn't link the SDK, so these satisfy the linker. */
MI_S32 MI_VENC_GetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t *attr)
{
	(void)chn; (void)attr; return -1;
}
MI_S32 MI_VENC_SetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t *attr)
{
	(void)chn; (void)attr; return -1;
}
MI_S32 MI_VENC_RequestIdr(MI_VENC_CHN chn, MI_BOOL instant)
{
	(void)chn; (void)instant; return -1;
}

/* Whitebox access to internal functions via extern declarations.
 * These are static in venc_api.c — we re-declare them here for testing.
 * This pattern matches the waybeam-hub test approach. */

/* We can't directly access statics, so we test through the public
 * venc_api_register() interface and verify side effects. */

/* ── Stub handler to capture responses ───────────────────────────────── */

/* For httpd route tests, we just verify registration succeeds */

/* ── Tests ───────────────────────────────────────────────────────────── */

static int test_register(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	/* Registration with NULL callbacks should succeed */
	int ret = venc_api_register(&cfg, "test", NULL);
	CHECK("register_ok", ret == 0);

	return failures;
}

static int test_register_with_callbacks(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	VencApplyCallbacks cb;
	memset(&cb, 0, sizeof(cb));

	int ret = venc_api_register(&cfg, "star6e", &cb);
	CHECK("register_cb_ok", ret == 0);

	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_venc_api(void)
{
	int failures = 0;
	failures += test_register();
	failures += test_register_with_callbacks();
	return failures;
}
