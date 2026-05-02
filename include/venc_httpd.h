#ifndef VENC_HTTPD_H
#define VENC_HTTPD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum sizes for parsed HTTP requests */
#define HTTPD_MAX_METHOD    8
#define HTTPD_MAX_PATH      256
#define HTTPD_MAX_QUERY     512
#define HTTPD_MAX_BODY      8192
#define HTTPD_MAX_ROUTES    64

/* Parsed HTTP request passed to route handlers */
typedef struct {
	char method[HTTPD_MAX_METHOD];
	char path[HTTPD_MAX_PATH];
	char query[HTTPD_MAX_QUERY];
	char body[HTTPD_MAX_BODY];
	int body_len;
} HttpRequest;

/* Handler function signature.  Writes response to client_fd.
 * Return 0 on success (response already sent), -1 on error. */
typedef int (*httpd_handler_fn)(int client_fd, const HttpRequest *req, void *ctx);

/* Register a route.  method is "GET", "POST", "PATCH", etc.
 * path_prefix is matched as a prefix (e.g. "/api/v1/config").
 * ctx is passed through to the handler. */
int venc_httpd_route(const char *method, const char *path_prefix,
	httpd_handler_fn handler, void *ctx);

/* Start the HTTP server on the given port.  Spawns a listener thread.
 * Returns 0 on success, -1 on error. */
int venc_httpd_start(uint16_t port);

/* Stop the HTTP server.  Closes socket and detaches the listener thread. */
void venc_httpd_stop(void);

/* ── Response helpers ────────────────────────────────────────────────── */

/* Send a raw HTTP response with the given status code and JSON body string. */
int httpd_send_json(int client_fd, int status_code, const char *json_str);

/* Send a plain-text response body. */
int httpd_send_text(int client_fd, int status_code, const char *text_str);

/* Send an HTML response body. */
int httpd_send_html(int client_fd, int status_code, const char *html_str);

/* Send a pre-compressed gzip HTML response. */
int httpd_send_html_gz(int client_fd, int status_code,
	const void *gz_data, int gz_len);

/* Send a JSON success envelope: {"ok":true,"data":{...}} */
int httpd_send_ok(int client_fd, const char *data_json);

/* Send a JSON error envelope: {"ok":false,"error":{"code":"...","message":"..."}} */
int httpd_send_error(int client_fd, int status_code,
	const char *code, const char *message);

/* Extract a named parameter from req->query, URL-decoding the value
 * (percent-encoded octets and '+' → ' ').  Writes NUL-terminated value
 * into out (truncated if needed).  Returns 0 if the key was present,
 * -1 otherwise. */
int httpd_query_param(const HttpRequest *req, const char *key,
	char *out, size_t out_sz);

/* Stream a file as an HTTP response with the given content type.  When
 * download_name is non-NULL, adds a Content-Disposition attachment
 * header with the given filename.  Responds with 404 if the file does
 * not exist, 500 on open/read failure.  Returns 0 on success, -1 on
 * error (headers or body failed to send). */
int httpd_send_file(int client_fd, const char *path,
	const char *content_type, const char *download_name);

/* Parse Content-Length anchored at the start of a header line.  An
 * unanchored substring search would also match values like
 *   X-Foo: content-length: 9999
 * which is a request-smuggling vector.  headers_start points to the
 * first byte of the first header line.  headers_end points to the
 * byte just past the last header line's terminator (i.e. the first
 * byte of the trailing empty CRLF that delimits the body).  Returns
 * the parsed value, clamped to [0, HTTPD_MAX_BODY-1]; returns 0 if
 * no Content-Length header is present.  Exposed for unit testing. */
int httpd_parse_content_length(const char *headers_start,
	const char *headers_end);

#ifdef __cplusplus
}
#endif

#endif /* VENC_HTTPD_H */
