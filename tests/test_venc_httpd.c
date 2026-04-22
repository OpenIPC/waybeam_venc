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

static int test_query_param(void)
{
	int failures = 0;
	HttpRequest req;
	char out[64];

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=foo.ts");
	CHECK("qp_found_simple",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		strcmp(out, "foo.ts") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "a=1&file=bar.ts&b=2");
	CHECK("qp_middle_key",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		strcmp(out, "bar.ts") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=hello%%20world");
	CHECK("qp_percent_space",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		strcmp(out, "hello world") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=a+b+c");
	CHECK("qp_plus_space",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		strcmp(out, "a b c") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=%%C3%%A9.ts");
	CHECK("qp_utf8_decode",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		(unsigned char)out[0] == 0xC3 &&
		(unsigned char)out[1] == 0xA9 &&
		strcmp(out + 2, ".ts") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "other=x");
	CHECK("qp_missing",
		httpd_query_param(&req, "file", out, sizeof(out)) == -1);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "filepart=bad&file=ok");
	CHECK("qp_prefix_collision",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		strcmp(out, "ok") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=");
	CHECK("qp_empty_value",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		out[0] == '\0');

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=bad%%Ghex");
	CHECK("qp_invalid_percent_passthrough",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		strcmp(out, "bad%Ghex") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=%%");
	CHECK("qp_trailing_percent",
		httpd_query_param(&req, "file", out, sizeof(out)) == 0 &&
		strcmp(out, "%") == 0);

	memset(&req, 0, sizeof(req));
	snprintf(req.query, sizeof(req.query), "file=verylongvalue");
	char small[8];
	CHECK("qp_truncate_safe",
		httpd_query_param(&req, "file", small, sizeof(small)) == 0 &&
		strlen(small) == sizeof(small) - 1 &&
		strncmp(small, "verylon", 7) == 0);

	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_venc_httpd(void)
{
	int failures = 0;
	failures += test_route_registration();
	failures += test_route_overflow();
	failures += test_request_struct_sizes();
	failures += test_query_param();
	return failures;
}
