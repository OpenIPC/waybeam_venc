/*
 * Unit tests for venc_api.c
 *
 * Tests: field descriptor lookup, registration, mutability,
 * field serialization/deserialization.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

typedef struct {
	int apply_bitrate_calls;
	int apply_fps_calls;
	int apply_gop_calls;
	int apply_qp_delta_calls;
	int apply_verbose_calls;
	int apply_awb_mode_calls;

	uint32_t last_bitrate;
	uint32_t last_fps;
	uint32_t last_gop;
	int last_qp_delta;
	bool last_verbose;
	int last_awb_mode;
	uint32_t last_awb_ct;

	int fail_bitrate;
	int fail_verbose;
	int fail_fps;
	int fail_gop;
} ApiCallbackState;

static ApiCallbackState g_api_cb_state;
static uint16_t g_api_test_port;
static int g_api_test_server_started;

static void reset_api_cb_state(void)
{
	memset(&g_api_cb_state, 0, sizeof(g_api_cb_state));
}

static uint16_t reserve_test_port(void)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	uint16_t port = 0;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(0);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
	    getsockname(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
		port = ntohs(addr.sin_port);
	}

	close(fd);
	return port;
}

static int ensure_api_test_server(void)
{
	if (g_api_test_server_started)
		return 0;

	g_api_test_port = reserve_test_port();
	if (g_api_test_port == 0)
		return -1;
	if (venc_httpd_start(g_api_test_port) != 0) {
		g_api_test_port = 0;
		return -1;
	}

	g_api_test_server_started = 1;
	return 0;
}

static void stop_api_test_server(void)
{
	if (!g_api_test_server_started)
		return;

	venc_httpd_stop();
	g_api_test_server_started = 0;
	g_api_test_port = 0;
}

static int connect_api_test_socket(void)
{
	struct sockaddr_in addr;
	int fd;
	int attempt;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(g_api_test_port);

	for (attempt = 0; attempt < 50; attempt++) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;

		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			return fd;

		close(fd);
		if (errno != ECONNREFUSED && errno != ENOENT)
			break;
		usleep(10000);
	}

	return -1;
}

static int read_http_response(int fd, int *http_status, char *response_buf,
	size_t response_buf_size)
{
	char raw[8192];
	char *body;
	size_t used = 0;
	ssize_t n;

	while (used < sizeof(raw) - 1) {
		n = read(fd, raw + used, sizeof(raw) - 1 - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	raw[used] = '\0';

	if (http_status) {
		int status = 0;
		if (sscanf(raw, "HTTP/%*u.%*u %d", &status) != 1)
			return -1;
		*http_status = status;
	}

	if (response_buf && response_buf_size > 0) {
		body = strstr(raw, "\r\n\r\n");
		if (!body)
			return -1;
		snprintf(response_buf, response_buf_size, "%s", body + 4);
	}

	return 0;
}

static int apply_set_query_http(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb, const char *query, int *http_status,
	char *response_buf, size_t response_buf_size)
{
	char request[1024];
	int fd;
	size_t sent = 0;
	size_t req_len;

	if (venc_api_register(cfg, backend_name, cb) != 0)
		return -1;
	if (ensure_api_test_server() != 0)
		return -1;

	fd = connect_api_test_socket();
	if (fd < 0)
		return -1;

	req_len = (size_t)snprintf(request, sizeof(request),
		"GET /api/v1/set?%s HTTP/1.0\r\n"
		"Host: 127.0.0.1\r\n"
		"\r\n",
		query ? query : "");
	if (req_len >= sizeof(request)) {
		close(fd);
		return -1;
	}

	while (sent < req_len) {
		ssize_t nwrite = write(fd, request + sent, req_len - sent);
		if (nwrite < 0 && errno == EINTR)
			continue;
		if (nwrite <= 0) {
			close(fd);
			return -1;
		}
		sent += (size_t)nwrite;
	}

	shutdown(fd, SHUT_WR);
	if (read_http_response(fd, http_status, response_buf,
	    response_buf_size) != 0) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static int test_apply_bitrate(uint32_t kbps)
{
	g_api_cb_state.apply_bitrate_calls++;
	g_api_cb_state.last_bitrate = kbps;
	return g_api_cb_state.fail_bitrate ? -1 : 0;
}

static int test_apply_fps(uint32_t fps)
{
	g_api_cb_state.apply_fps_calls++;
	g_api_cb_state.last_fps = fps;
	return g_api_cb_state.fail_fps ? -1 : 0;
}

static int test_apply_gop(uint32_t gop_size)
{
	g_api_cb_state.apply_gop_calls++;
	g_api_cb_state.last_gop = gop_size;
	return g_api_cb_state.fail_gop ? -1 : 0;
}

static int test_apply_qp_delta(int delta)
{
	g_api_cb_state.apply_qp_delta_calls++;
	g_api_cb_state.last_qp_delta = delta;
	return 0;
}

static int test_apply_verbose(bool on)
{
	g_api_cb_state.apply_verbose_calls++;
	g_api_cb_state.last_verbose = on;
	return g_api_cb_state.fail_verbose ? -1 : 0;
}

static int test_apply_awb_mode(int mode, uint32_t ct)
{
	g_api_cb_state.apply_awb_mode_calls++;
	g_api_cb_state.last_awb_mode = mode;
	g_api_cb_state.last_awb_ct = ct;
	return 0;
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

static int test_field_support_by_backend(void)
{
	int failures = 0;

	CHECK("enc_ctrl supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"enc_ctrl.enabled") == 1);
	CHECK("enc_ctrl alias supported star6e",
		venc_api_field_supported_for_backend("star6e",
			"encCtrl.maxGopSize") == 1);
	CHECK("enc_ctrl unsupported maruko",
		venc_api_field_supported_for_backend("maruko",
			"enc_ctrl.enabled") == 0);
	CHECK("enc_ctrl unsupported unknown",
		venc_api_field_supported_for_backend("test",
			"encCtrl.sceneChangeThreshold") == 0);
	CHECK("regular field supported maruko",
		venc_api_field_supported_for_backend("maruko",
			"video0.bitrate") == 1);

	return failures;
}

static int test_multi_set_live_success(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;
	cb.apply_verbose = test_apply_verbose;

	CHECK("multi set apply ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("multi set status 200", status == 200);
	CHECK("multi set bitrate cfg", cfg.video0.bitrate == 4096);
	CHECK("multi set verbose cfg", cfg.system.verbose == true);
	CHECK("multi set bitrate applied once", g_api_cb_state.apply_bitrate_calls == 1);
	CHECK("multi set verbose applied once", g_api_cb_state.apply_verbose_calls == 1);
	CHECK("multi set bitrate value", g_api_cb_state.last_bitrate == 4096);
	CHECK("multi set verbose value", g_api_cb_state.last_verbose == true);
	CHECK("multi set response array", strstr(response, "\"applied\"") != NULL);

	return failures;
}

static int test_multi_set_awb_grouped_apply(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_awb_mode = test_apply_awb_mode;

	CHECK("multi awb apply ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"isp.awbMode=ct_manual&isp.awbCt=6000",
			&status, response, sizeof(response)) == 0);
	CHECK("multi awb status 200", status == 200);
	CHECK("multi awb mode cfg", strcmp(cfg.isp.awb_mode, "ct_manual") == 0);
	CHECK("multi awb ct cfg", cfg.isp.awb_ct == 6000);
	CHECK("multi awb grouped once", g_api_cb_state.apply_awb_mode_calls == 1);
	CHECK("multi awb mode value", g_api_cb_state.last_awb_mode == 1);
	CHECK("multi awb ct value", g_api_cb_state.last_awb_ct == 6000);
	CHECK("multi awb response alias", strstr(response, "isp.awbMode") != NULL);

	return failures;
}

static int test_multi_set_video_timing_grouped_apply(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_fps = test_apply_fps;
	cb.apply_gop = test_apply_gop;

	CHECK("multi timing apply ok",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.fps=30&video0.gopSize=1.0",
			&status, response, sizeof(response)) == 0);
	CHECK("multi timing status 200", status == 200);
	CHECK("multi timing fps cfg", cfg.video0.fps == 30);
	CHECK("multi timing gop cfg", cfg.video0.gop_size == 1.0);
	CHECK("multi timing fps once", g_api_cb_state.apply_fps_calls == 1);
	CHECK("multi timing gop once", g_api_cb_state.apply_gop_calls == 1);
	CHECK("multi timing fps value", g_api_cb_state.last_fps == 30);
	CHECK("multi timing gop value", g_api_cb_state.last_gop == 30);
	CHECK("multi timing response alias", strstr(response, "video0.gopSize") != NULL);

	return failures;
}

static int test_multi_set_rejects_restart_fields(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];
	uint32_t old_bitrate;
	uint32_t old_width;
	uint32_t old_height;

	venc_config_defaults(&cfg);
	old_bitrate = cfg.video0.bitrate;
	old_width = cfg.video0.width;
	old_height = cfg.video0.height;
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;

	CHECK("multi reject restart rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&video0.size=1280x720",
			&status, response, sizeof(response)) == 0);
	CHECK("multi reject restart status", status == 400);
	CHECK("multi reject restart error",
		strstr(response, "multi-set only supports live fields") != NULL);
	CHECK("multi reject restart bitrate unchanged", cfg.video0.bitrate == old_bitrate);
	CHECK("multi reject restart width unchanged", cfg.video0.width == old_width);
	CHECK("multi reject restart height unchanged", cfg.video0.height == old_height);
	CHECK("multi reject restart no callbacks", g_api_cb_state.apply_bitrate_calls == 0);

	return failures;
}

static int test_multi_set_rejects_duplicate_fields(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_qp_delta = test_apply_qp_delta;

	CHECK("multi dup rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.qp_delta=1&video0.qpDelta=2",
			&status, response, sizeof(response)) == 0);
	CHECK("multi dup status", status == 400);
	CHECK("multi dup error", strstr(response, "duplicate field") != NULL);
	CHECK("multi dup qp unchanged", cfg.video0.qp_delta == -4);
	CHECK("multi dup no apply", g_api_cb_state.apply_qp_delta_calls == 0);

	return failures;
}

static int test_multi_set_preflights_missing_callback(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;

	CHECK("multi preflight rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("multi preflight status", status == 501);
	CHECK("multi preflight error",
		strstr(response, "apply callback not available") != NULL);
	CHECK("multi preflight bitrate unchanged", cfg.video0.bitrate == 8192);
	CHECK("multi preflight verbose unchanged", cfg.system.verbose == false);
	CHECK("multi preflight no side effects", g_api_cb_state.apply_bitrate_calls == 0);

	return failures;
}

static int test_multi_set_rolls_back_on_apply_failure(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_bitrate = test_apply_bitrate;
	cb.apply_verbose = test_apply_verbose;
	g_api_cb_state.fail_verbose = 1;

	CHECK("multi rollback rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.bitrate=4096&system.verbose=true",
			&status, response, sizeof(response)) == 0);
	CHECK("multi rollback status", status == 500);
	CHECK("multi rollback error",
		strstr(response, "failed to apply live field group") != NULL);
	CHECK("multi rollback bitrate restored", cfg.video0.bitrate == 8192);
	CHECK("multi rollback verbose restored", cfg.system.verbose == false);
	CHECK("multi rollback bitrate forward+rollback",
		g_api_cb_state.apply_bitrate_calls == 2);
	CHECK("multi rollback verbose attempted+rollback",
		g_api_cb_state.apply_verbose_calls == 2);
	CHECK("multi rollback bitrate restored value",
		g_api_cb_state.last_bitrate == 8192);
	CHECK("multi rollback verbose restored value",
		g_api_cb_state.last_verbose == false);

	return failures;
}

static int test_single_set_runtime_apply_failure(void)
{
	int failures = 0;
	VencConfig cfg;
	VencApplyCallbacks cb;
	int status = 0;
	char response[1024];
	uint32_t old_fps;
	double old_gop_size;

	venc_config_defaults(&cfg);
	old_fps = cfg.video0.fps;
	old_gop_size = cfg.video0.gop_size;
	reset_api_cb_state();
	memset(&cb, 0, sizeof(cb));
	cb.apply_fps = test_apply_fps;
	cb.apply_gop = test_apply_gop;
	g_api_cb_state.fail_gop = 1;

	CHECK("single failure rc",
		apply_set_query_http(&cfg, "star6e", &cb,
			"video0.fps=30", &status, response,
			sizeof(response)) == 0);
	CHECK("single failure status", status == 500);
	CHECK("single failure error",
		strstr(response, "failed to apply live field group") != NULL);
	CHECK("single failure fps restored", cfg.video0.fps == old_fps);
	CHECK("single failure gop restored", cfg.video0.gop_size == old_gop_size);
	CHECK("single failure fps forward+rollback",
		g_api_cb_state.apply_fps_calls == 2);
	CHECK("single failure gop attempted+rollback",
		g_api_cb_state.apply_gop_calls == 2);
	CHECK("single failure fps restored value",
		g_api_cb_state.last_fps == old_fps);

	return failures;
}

static int test_live_set_rejects_out_of_range_roi_values(void)
{
	int failures = 0;
	VencConfig cfg;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);

	CHECK("roi center reject rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"fpv.roi_center=42", &status, response,
			sizeof(response)) == 0);
	CHECK("roi center reject status", status == 409);
	CHECK("roi center reject error",
		strstr(response, "roi_center must be in range [0.1, 0.9]") != NULL);
	CHECK("roi center unchanged", cfg.fpv.roi_center == 0.4);

	CHECK("roi steps reject rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"fpv.roi_steps=999", &status, response,
			sizeof(response)) == 0);
	CHECK("roi steps reject status", status == 409);
	CHECK("roi steps reject error",
		strstr(response, "roi_steps must be in range [1, 4]") != NULL);
	CHECK("roi steps unchanged", cfg.fpv.roi_steps == 2);

	return failures;
}

static int test_restart_set_accepts_maruko_h264_config(void)
{
	int failures = 0;
	VencConfig cfg;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	strcpy(cfg.video0.codec, "h264");

	CHECK("maruko restart h264 rc",
		apply_set_query_http(&cfg, "maruko", NULL,
			"video0.size=1280x720", &status, response,
			sizeof(response)) == 0);
	CHECK("maruko restart h264 status", status == 200);
	CHECK("maruko restart width updated", cfg.video0.width == 1280);
	CHECK("maruko restart height updated", cfg.video0.height == 720);
	CHECK("maruko restart response reinit",
		strstr(response, "\"reinit_pending\":true") != NULL);

	return failures;
}

static int test_restart_set_rejects_star6e_h264_rtp(void)
{
	int failures = 0;
	VencConfig cfg;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);

	CHECK("star6e restart h264 rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"video0.codec=h264", &status, response,
			sizeof(response)) == 0);
	CHECK("star6e restart h264 status", status == 409);
	CHECK("star6e restart h264 unchanged",
		strcmp(cfg.video0.codec, "h265") == 0);
	CHECK("star6e restart h264 error",
		strstr(response, "star6e RTP mode currently supports h265 only") != NULL);

	return failures;
}

static int test_restart_set_accepts_star6e_h264_compact(void)
{
	int failures = 0;
	VencConfig cfg;
	int status = 0;
	char response[1024];

	venc_config_defaults(&cfg);
	strcpy(cfg.outgoing.stream_mode, "compact");

	CHECK("star6e compact h264 rc",
		apply_set_query_http(&cfg, "star6e", NULL,
			"video0.codec=h264", &status, response,
			sizeof(response)) == 0);
	CHECK("star6e compact h264 status", status == 200);
	CHECK("star6e compact h264 updated",
		strcmp(cfg.video0.codec, "h264") == 0);
	CHECK("star6e compact h264 response reinit",
		strstr(response, "\"reinit_pending\":true") != NULL);

	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_venc_api(void)
{
	int failures = 0;
	failures += test_register();
	failures += test_register_with_callbacks();
	failures += test_field_support_by_backend();
	failures += test_multi_set_live_success();
	failures += test_multi_set_awb_grouped_apply();
	failures += test_multi_set_video_timing_grouped_apply();
	failures += test_multi_set_rejects_restart_fields();
	failures += test_multi_set_rejects_duplicate_fields();
	failures += test_multi_set_preflights_missing_callback();
	failures += test_multi_set_rolls_back_on_apply_failure();
	failures += test_single_set_runtime_apply_failure();
	failures += test_live_set_rejects_out_of_range_roi_values();
	failures += test_restart_set_accepts_maruko_h264_config();
	failures += test_restart_set_rejects_star6e_h264_rtp();
	failures += test_restart_set_accepts_star6e_h264_compact();
	stop_api_test_server();
	return failures;
}
