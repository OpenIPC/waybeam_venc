#include "venc_httpd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Case-insensitive substring search (portable, no _GNU_SOURCE needed). */
static char *httpd_strcasestr(const char *haystack, const char *needle)
{
	size_t nlen = strlen(needle);
	if (nlen == 0) return (char *)haystack;
	for (; *haystack; haystack++) {
		if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle) &&
		    strncasecmp(haystack, needle, nlen) == 0)
			return (char *)haystack;
	}
	return NULL;
}

/* ── Route table ─────────────────────────────────────────────────────── */

typedef struct {
	char method[HTTPD_MAX_METHOD];
	char path_prefix[HTTPD_MAX_PATH];
	httpd_handler_fn handler;
	void *ctx;
} HttpRoute;

static HttpRoute g_routes[HTTPD_MAX_ROUTES];
static int g_route_count = 0;

int venc_httpd_route(const char *method, const char *path_prefix,
	httpd_handler_fn handler, void *ctx)
{
	if (g_route_count >= HTTPD_MAX_ROUTES) {
		fprintf(stderr, "[httpd] ERROR: route table full\n");
		return -1;
	}
	HttpRoute *r = &g_routes[g_route_count++];
	snprintf(r->method, sizeof(r->method), "%s", method);
	snprintf(r->path_prefix, sizeof(r->path_prefix), "%s", path_prefix);
	r->handler = handler;
	r->ctx = ctx;
	return 0;
}

/* ── Response helpers ────────────────────────────────────────────────── */

static int write_socket_all(int fd, const char *buf, int len)
{
	int total = 0;

	while (total < len) {
		int n = (int)send(fd, buf + total, (size_t)(len - total),
			MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		total += n;
	}

	return 0;
}

static int send_response(int fd, int status, const char *status_text,
	const char *content_type, const char *body, int body_len)
{
	char header[512];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n",
		status, status_text, content_type, body_len);

	if (write_socket_all(fd, header, hlen) != 0)
		return -1;
	if (write_socket_all(fd, body, body_len) != 0)
		return -1;

	return 0;
}

static const char *status_text(int code)
{
	switch (code) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	default:  return "Unknown";
	}
}

int httpd_send_json(int client_fd, int status_code, const char *json_str)
{
	return send_response(client_fd, status_code, status_text(status_code),
		"application/json", json_str, (int)strlen(json_str));
}

int httpd_send_text(int client_fd, int status_code, const char *text_str)
{
	return send_response(client_fd, status_code, status_text(status_code),
		"text/plain; charset=utf-8", text_str, (int)strlen(text_str));
}

int httpd_send_html(int client_fd, int status_code, const char *html_str)
{
	return send_response(client_fd, status_code, status_text(status_code),
		"text/html; charset=utf-8", html_str, (int)strlen(html_str));
}

int httpd_send_html_gz(int client_fd, int status_code,
	const void *gz_data, int gz_len)
{
	char header[512];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Encoding: gzip\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n",
		status_code, status_text(status_code), gz_len);
	if (write_socket_all(client_fd, header, hlen) != 0)
		return -1;
	if (write_socket_all(client_fd, (const char *)gz_data, gz_len) != 0)
		return -1;
	return 0;
}

int httpd_send_ok(int client_fd, const char *data_json)
{
	char buf[2048];
	int len;
	if (data_json && data_json[0]) {
		len = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":%s}", data_json);
	} else {
		len = snprintf(buf, sizeof(buf), "{\"ok\":true}");
	}
	if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
	return httpd_send_json(client_fd, 200, buf);
}

int httpd_send_error(int client_fd, int status_code,
	const char *code, const char *message)
{
	char buf[1024];
	int len = snprintf(buf, sizeof(buf),
		"{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
		code ? code : "internal_error",
		message ? message : "unknown error");
	if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
	return httpd_send_json(client_fd, status_code, buf);
}

/* ── Request parsing ─────────────────────────────────────────────────── */

static int parse_request(int fd, HttpRequest *req)
{
	memset(req, 0, sizeof(*req));

	/* Read the full request into a buffer.  HTTP/1.0, small payloads. */
	char raw[HTTPD_MAX_BODY + 2048];
	int total = 0;
	while (total < (int)sizeof(raw) - 1) {
		int n = (int)read(fd, raw + total, (size_t)(sizeof(raw) - 1 - (size_t)total));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		total += n;
		/* Check for end of headers (body handled via Content-Length) */
		if (total >= 4 && memmem(raw, (size_t)total, "\r\n\r\n", 4))
			break;
	}
	if (total <= 0) return -1;
	raw[total] = '\0';

	/* Parse request line: METHOD /path?query HTTP/1.x */
	char *line_end = strstr(raw, "\r\n");
	if (!line_end) return -1;
	*line_end = '\0';

	char *sp1 = strchr(raw, ' ');
	if (!sp1) return -1;
	*sp1 = '\0';
	size_t mlen = (size_t)(sp1 - raw);
	if (mlen >= sizeof(req->method))
		mlen = sizeof(req->method) - 1;
	memcpy(req->method, raw, mlen);
	req->method[mlen] = '\0';

	char *uri = sp1 + 1;
	char *sp2 = strchr(uri, ' ');
	if (sp2) *sp2 = '\0';

	/* Split URI into path and query */
	char *qmark = strchr(uri, '?');
	if (qmark) {
		*qmark = '\0';
		snprintf(req->query, sizeof(req->query), "%s", qmark + 1);
	}
	snprintf(req->path, sizeof(req->path), "%s", uri);

	/* Parse Content-Length and read body if present */
	char *headers_start = line_end + 2;
	char *body_start = strstr(headers_start, "\r\n\r\n");
	if (body_start) {
		body_start += 4;
		/* Find Content-Length */
		char *cl = httpd_strcasestr(headers_start, "content-length:");
		int content_len = 0;
		if (cl) {
			content_len = atoi(cl + 15);
			if (content_len < 0) content_len = 0;
			if (content_len >= HTTPD_MAX_BODY) content_len = HTTPD_MAX_BODY - 1;
		}

		/* Copy what we already have */
		int already = total - (int)(body_start - raw);
		if (already < 0) already = 0;
		if (already > content_len) already = content_len;
		if (already > 0)
			memcpy(req->body, body_start, (size_t)already);

		/* Read remaining body if needed */
		while (already < content_len) {
			int n = (int)read(fd, req->body + already,
				(size_t)(content_len - already));
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) break;
			already += n;
		}
		req->body_len = already;
		req->body[already] = '\0';
	}

	return 0;
}

/* ── Route dispatch ──────────────────────────────────────────────────── */

static void dispatch(int client_fd, const HttpRequest *req)
{
	for (int i = 0; i < g_route_count; i++) {
		HttpRoute *r = &g_routes[i];
		if (strcmp(req->method, r->method) != 0)
			continue;
		size_t pfx_len = strlen(r->path_prefix);
		if (strncmp(req->path, r->path_prefix, pfx_len) != 0)
			continue;
		/* Prefix matches — only if path ends here or continues with / or ? */
		char next = req->path[pfx_len];
		if (next != '\0' && next != '/' && next != '?')
			continue;
		r->handler(client_fd, req, r->ctx);
		return;
	}

	httpd_send_error(client_fd, 404, "not_found", "no matching route");
}

/* ── Server thread ───────────────────────────────────────────────────── */

static int g_listen_fd = -1;
static pthread_t g_thread;
static volatile int g_running = 0;

static void *httpd_thread(void *arg)
{
	(void)arg;

	while (g_running) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client_fd = accept(g_listen_fd,
			(struct sockaddr *)&client_addr, &addr_len);
		if (client_fd < 0) {
			if (g_running && errno != EINTR)
				fprintf(stderr, "[httpd] accept error: %s\n", strerror(errno));
			continue;
		}

		/* Set a read timeout so we don't hang on slow/malformed clients */
		struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
		setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		HttpRequest req;
		if (parse_request(client_fd, &req) == 0) {
			dispatch(client_fd, &req);
		}

		close(client_fd);
	}

	return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int venc_httpd_start(uint16_t port)
{
	if (g_running) {
		fprintf(stderr, "[httpd] already running\n");
		return -1;
	}

	g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_listen_fd < 0) {
		fprintf(stderr, "[httpd] socket error: %s\n", strerror(errno));
		return -1;
	}

	int opt = 1;
	setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[httpd] bind port %u error: %s\n", port, strerror(errno));
		close(g_listen_fd);
		g_listen_fd = -1;
		return -1;
	}

	if (listen(g_listen_fd, 4) < 0) {
		fprintf(stderr, "[httpd] listen error: %s\n", strerror(errno));
		close(g_listen_fd);
		g_listen_fd = -1;
		return -1;
	}

	g_running = 1;
	if (pthread_create(&g_thread, NULL, httpd_thread, NULL) != 0) {
		fprintf(stderr, "[httpd] pthread_create error: %s\n", strerror(errno));
		g_running = 0;
		close(g_listen_fd);
		g_listen_fd = -1;
		return -1;
	}

	fprintf(stderr, "[httpd] listening on port %u\n", port);
	return 0;
}

void venc_httpd_stop(void)
{
	if (!g_running)
		return;

	g_running = 0;

	/* Close the listen socket to unblock accept() */
	if (g_listen_fd >= 0) {
		shutdown(g_listen_fd, SHUT_RDWR);
		close(g_listen_fd);
		g_listen_fd = -1;
	}

	/* Detach instead of join — on some embedded kernels, closing the listen
	 * fd does not reliably unblock accept() in another thread, which would
	 * cause pthread_join to hang and block the entire shutdown sequence. */
	pthread_detach(g_thread);
	fprintf(stderr, "[httpd] stopped\n");
}
