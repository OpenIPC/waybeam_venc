/*
 * Unit tests for venc_httpd.c
 *
 * Tests: route registration, response helpers (via format validation).
 * Socket/threading tests are not included (would need integration harness).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "venc_httpd.h"
#include "test_helpers.h"

/* ── Dummy handler for route tests ───────────────────────────────────── */

static int dummy_handler(int fd, const HttpRequest *req, void *ctx)
{
	(void)fd; (void)req; (void)ctx;
	return 0;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static int test_route_registration(void)
{
	int failures = 0;

	int ret = venc_httpd_route("GET", "/test/route1", dummy_handler, NULL);
	CHECK("route1_ok", ret == 0);

	ret = venc_httpd_route("POST", "/test/route2", dummy_handler, NULL);
	CHECK("route2_ok", ret == 0);

	ret = venc_httpd_route("GET", "/test/route3", dummy_handler, (void *)0x42);
	CHECK("route3_ctx_ok", ret == 0);

	return failures;
}

static int test_route_overflow(void)
{
	int failures = 0;

	/* Register routes up to the limit.  Some slots are already taken by
	 * previous tests and by venc_api_register(), so we just verify that
	 * registration eventually fails gracefully. */
	int ok_count = 0;
	for (int i = 0; i < HTTPD_MAX_ROUTES + 5; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/overflow/%d", i);
		int ret = venc_httpd_route("GET", path, dummy_handler, NULL);
		if (ret == 0)
			ok_count++;
	}

	/* We should have hit the limit before HTTPD_MAX_ROUTES + 5 */
	CHECK("route_overflow_bounded", ok_count <= HTTPD_MAX_ROUTES);

	return failures;
}

static int test_request_struct_sizes(void)
{
	int failures = 0;

	/* Verify buffer size constants are reasonable */
	CHECK("max_method_sane", HTTPD_MAX_METHOD >= 4);
	CHECK("max_path_sane", HTTPD_MAX_PATH >= 128);
	CHECK("max_query_sane", HTTPD_MAX_QUERY >= 128);
	CHECK("max_body_sane", HTTPD_MAX_BODY >= 1024);

	/* HttpRequest struct can hold typical values */
	HttpRequest req;
	memset(&req, 0, sizeof(req));
	snprintf(req.method, sizeof(req.method), "GET");
	snprintf(req.path, sizeof(req.path), "/api/v1/config");
	snprintf(req.query, sizeof(req.query), "video0.bitrate=8192");
	CHECK("req_method", strcmp(req.method, "GET") == 0);
	CHECK("req_path", strcmp(req.path, "/api/v1/config") == 0);
	CHECK("req_query", strcmp(req.query, "video0.bitrate=8192") == 0);

	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_venc_httpd(void)
{
	int failures = 0;
	failures += test_route_registration();
	failures += test_route_overflow();
	failures += test_request_struct_sizes();
	return failures;
}
