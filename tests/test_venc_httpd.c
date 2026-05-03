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

/* ── Content-Length parser tests ─────────────────────────────────────── */

/* Helper: set up the (headers_start, headers_end) pair the way
 * parse_request() would, given a header block ending with the empty
 * terminator CRLF.  The block must include the trailing "\r\n\r\n". */
static int parse_cl(const char *headers_with_terminator)
{
	const char *body_start = strstr(headers_with_terminator, "\r\n\r\n");
	if (!body_start)
		return -999;
	body_start += 4;
	return httpd_parse_content_length(headers_with_terminator,
		body_start - 2);
}

static int test_content_length_parse(void)
{
	int failures = 0;

	CHECK("cl_basic",
		parse_cl("Content-Length: 42\r\n\r\n") == 42);

	CHECK("cl_lowercase",
		parse_cl("content-length: 7\r\n\r\n") == 7);

	CHECK("cl_mixed_case",
		parse_cl("CoNtEnT-LeNgTh: 13\r\n\r\n") == 13);

	CHECK("cl_no_space",
		parse_cl("Content-Length:99\r\n\r\n") == 99);

	CHECK("cl_multi_whitespace",
		parse_cl("Content-Length:   123\r\n\r\n") == 123);

	CHECK("cl_tab_whitespace",
		parse_cl("Content-Length:\t456\r\n\r\n") == 456);

	CHECK("cl_missing_returns_zero",
		parse_cl("Host: example.com\r\nUser-Agent: test\r\n\r\n") == 0);

	CHECK("cl_negative_clamped_to_zero",
		parse_cl("Content-Length: -5\r\n\r\n") == 0);

	CHECK("cl_oversized_clamped",
		parse_cl("Content-Length: 99999999\r\n\r\n") ==
			HTTPD_MAX_BODY - 1);

	CHECK("cl_at_max_minus_one",
		parse_cl("Content-Length: 8191\r\n\r\n") == HTTPD_MAX_BODY - 1);

	CHECK("cl_at_max_clamped",
		parse_cl("Content-Length: 8192\r\n\r\n") == HTTPD_MAX_BODY - 1);

	/* Smuggling: header value contains the literal "content-length:".
	 * The pre-fix parser would latch onto the substring inside the value
	 * and forge a body length from "9999". */
	CHECK("cl_smuggling_header_value_ignored",
		parse_cl("Host: example.com\r\n"
		         "X-Forwarded-By: content-length: 9999\r\n\r\n") == 0);

	/* Smuggling variant: the literal sits in the request URI part of
	 * the buffer (mimicking attacker-controlled body bytes appearing
	 * before the real headers).  We still expect zero. */
	CHECK("cl_smuggling_value_with_real_header_first",
		parse_cl("X-Note: see content-length: 100 below\r\n"
		         "Accept: */*\r\n\r\n") == 0);

	/* Real C-L wins when both substring-trap and a valid header exist. */
	CHECK("cl_real_header_after_value_trap",
		parse_cl("X-Note: content-length: 9999\r\n"
		         "Content-Length: 17\r\n\r\n") == 17);

	/* Multiple headers preceding the C-L line. */
	CHECK("cl_after_other_headers",
		parse_cl("Host: example.com\r\n"
		         "User-Agent: test\r\n"
		         "Accept: */*\r\n"
		         "Content-Length: 33\r\n\r\n") == 33);

	/* First-and-only header. */
	CHECK("cl_first_line",
		parse_cl("Content-Length: 1\r\n\r\n") == 1);

	/* Zero value is preserved (a real client can send C-L: 0). */
	CHECK("cl_zero_value",
		parse_cl("Content-Length: 0\r\n\r\n") == 0);

	/* Empty header block (request line + immediate body terminator).
	 * Real callers won't reach this branch because parse_request only
	 * invokes the parser after spotting "\r\n\r\n", but defensively
	 * the helper must return 0 instead of UB. */
	CHECK("cl_empty_block", parse_cl("\r\n\r\n") == 0);

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
	failures += test_content_length_parse();
	return failures;
}
