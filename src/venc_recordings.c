#include "venc_recordings.h"

#include "venc_api.h"
#include "venc_httpd.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define REC_MAX_ENTRIES 512

/* ── helpers ──────────────────────────────────────────────────────────── */

static int has_ext(const char *name, const char *ext)
{
	size_t nlen = strlen(name);
	size_t elen = strlen(ext);
	return nlen > elen && strcasecmp(name + nlen - elen, ext) == 0;
}

static int is_recording(const char *name)
{
	return has_ext(name, ".ts") || has_ext(name, ".hevc");
}

/* Accept only plain filenames: non-empty, no path separators, no ".."
 * or leading dot, no control bytes. */
static int safe_filename(const char *name)
{
	if (!name || !*name) return 0;
	if (name[0] == '.') return 0;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		if (*p == '/' || *p == '\\' || *p < 0x20 || *p == 0x7f)
			return 0;
	}
	return 1;
}

static const char *content_type_for(const char *name)
{
	if (has_ext(name, ".ts")) return "video/mp2t";
	return "application/octet-stream";
}

/* Append one byte, growing the buffer if needed.  Returns 0 on success, -1
 * on OOM (caller should abort the response). */
static int dyn_putc(char **buf, size_t *cap, size_t *len, char c)
{
	if (*len + 1 >= *cap) {
		size_t new_cap = (*cap < 1024) ? 1024 : *cap * 2;
		char *p = realloc(*buf, new_cap);
		if (!p) return -1;
		*buf = p;
		*cap = new_cap;
	}
	(*buf)[(*len)++] = c;
	return 0;
}

static int dyn_puts(char **buf, size_t *cap, size_t *len, const char *s)
{
	while (*s) {
		if (dyn_putc(buf, cap, len, *s++) != 0) return -1;
	}
	return 0;
}

static int dyn_printf(char **buf, size_t *cap, size_t *len,
	const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static int dyn_printf(char **buf, size_t *cap, size_t *len,
	const char *fmt, ...)
{
	char tmp[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0) return -1;
	if ((size_t)n < sizeof(tmp))
		return dyn_puts(buf, cap, len, tmp);
	/* Oversize — allocate and try again. */
	size_t need = (size_t)n + 1;
	char *big = malloc(need);
	if (!big) return -1;
	va_start(ap, fmt);
	vsnprintf(big, need, fmt, ap);
	va_end(ap);
	int rc = dyn_puts(buf, cap, len, big);
	free(big);
	return rc;
}

/* Append a JSON string literal (including surrounding quotes), escaping
 * ", \, and control bytes per RFC 8259. */
static int dyn_put_json_str(char **buf, size_t *cap, size_t *len,
	const char *s)
{
	if (dyn_putc(buf, cap, len, '"') != 0) return -1;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			if (dyn_putc(buf, cap, len, '\\') != 0) return -1;
			if (dyn_putc(buf, cap, len, (char)*p) != 0) return -1;
		} else if (*p < 0x20) {
			char esc[8];
			snprintf(esc, sizeof(esc), "\\u%04x", *p);
			if (dyn_puts(buf, cap, len, esc) != 0) return -1;
		} else {
			if (dyn_putc(buf, cap, len, (char)*p) != 0) return -1;
		}
	}
	return dyn_putc(buf, cap, len, '"');
}

/* ── handlers ─────────────────────────────────────────────────────────── */

static int handle_list(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;

	char dir[256];
	venc_api_get_record_dir(dir, sizeof(dir));

	DIR *d = opendir(dir);
	if (!d) {
		int status = (errno == ENOENT) ? 503 : 500;
		const char *msg = (errno == ENOENT) ?
			"recording directory not mounted" :
			"cannot open recording directory";
		return httpd_send_error(fd, status, "not_available", msg);
	}

	/* Collect entries first (name + stat result) so the JSON we emit is a
	 * snapshot.  A bounded cap keeps memory predictable on a full SD. */
	struct {
		char name[256];
		long long size;
		long long mtime;
	} *entries = calloc(REC_MAX_ENTRIES, sizeof(*entries));
	if (!entries) {
		closedir(d);
		return httpd_send_error(fd, 500, "internal_error",
			"out of memory");
	}

	int count = 0;
	int truncated = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (!is_recording(de->d_name)) continue;
		if (count >= REC_MAX_ENTRIES) { truncated = 1; break; }
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		snprintf(entries[count].name, sizeof(entries[count].name),
			"%s", de->d_name);
		entries[count].size = (long long)st.st_size;
		entries[count].mtime = (long long)st.st_mtime;
		count++;
	}
	closedir(d);

	long long free_bytes = -1;
	long long total_bytes = -1;
	struct statvfs svfs;
	if (statvfs(dir, &svfs) == 0) {
		free_bytes = (long long)svfs.f_bavail * (long long)svfs.f_frsize;
		total_bytes = (long long)svfs.f_blocks * (long long)svfs.f_frsize;
	}

	char *buf = NULL;
	size_t cap = 0, len = 0;
	int err = 0;

	err |= dyn_puts(&buf, &cap, &len, "{\"ok\":true,\"data\":{\"dir\":");
	err |= dyn_put_json_str(&buf, &cap, &len, dir);
	err |= dyn_printf(&buf, &cap, &len,
		",\"free_bytes\":%lld,\"total_bytes\":%lld,\"files\":[",
		free_bytes, total_bytes);
	for (int i = 0; i < count && !err; i++) {
		if (i > 0) err |= dyn_putc(&buf, &cap, &len, ',');
		err |= dyn_puts(&buf, &cap, &len, "{\"name\":");
		err |= dyn_put_json_str(&buf, &cap, &len, entries[i].name);
		err |= dyn_printf(&buf, &cap, &len,
			",\"size\":%lld,\"mtime\":%lld}",
			entries[i].size, entries[i].mtime);
	}
	err |= dyn_printf(&buf, &cap, &len, "],\"truncated\":%s}}",
		truncated ? "true" : "false");
	err |= dyn_putc(&buf, &cap, &len, '\0');

	free(entries);
	if (err || !buf) {
		free(buf);
		return httpd_send_error(fd, 500, "internal_error",
			"out of memory");
	}

	int rc = httpd_send_json(fd, 200, buf);
	free(buf);
	return rc;
}

static int handle_download(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	char name[256];
	if (httpd_query_param(req, "file", name, sizeof(name)) != 0 ||
	    !safe_filename(name)) {
		return httpd_send_error(fd, 400, "invalid_request",
			"missing or invalid file parameter");
	}

	char dir[256];
	venc_api_get_record_dir(dir, sizeof(dir));

	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);

	return httpd_send_file(fd, path, content_type_for(name), name);
}

static int handle_delete(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	char name[256];
	if (httpd_query_param(req, "file", name, sizeof(name)) != 0 ||
	    !safe_filename(name)) {
		return httpd_send_error(fd, 400, "invalid_request",
			"missing or invalid file parameter");
	}

	char dir[256];
	venc_api_get_record_dir(dir, sizeof(dir));

	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);

	struct stat st;
	if (stat(path, &st) != 0) {
		int status = (errno == ENOENT) ? 404 : 500;
		const char *code = (errno == ENOENT) ? "not_found" : "internal_error";
		const char *msg = (errno == ENOENT) ? "file not found" :
			"cannot stat file";
		return httpd_send_error(fd, status, code, msg);
	}

	/* Refuse to delete the currently-recording file by comparing inodes —
	 * a string compare of paths would be defeated by trailing slashes or
	 * symlinks. */
	VencRecordStatus rec;
	venc_api_fill_record_status(&rec);
	if (rec.active && rec.path[0]) {
		struct stat rst;
		if (stat(rec.path, &rst) == 0 &&
		    rst.st_dev == st.st_dev && rst.st_ino == st.st_ino) {
			return httpd_send_error(fd, 409, "record_active",
				"file is being recorded; stop recording first");
		}
	}

	if (unlink(path) != 0) {
		return httpd_send_error(fd, 500, "delete_failed", "delete failed");
	}

	return httpd_send_ok(fd, NULL);
}

/* ── registration ─────────────────────────────────────────────────────── */

int venc_recordings_register(void)
{
	int rc = 0;
	/* More-specific paths first: httpd dispatches on the first matching
	 * prefix, so /api/v1/recordings/download must register before
	 * /api/v1/recordings. */
	rc |= venc_httpd_route("GET", "/api/v1/recordings/download",
		handle_download, NULL);
	rc |= venc_httpd_route("GET", "/api/v1/recordings/delete",
		handle_delete, NULL);
	rc |= venc_httpd_route("GET", "/api/v1/recordings",
		handle_list, NULL);
	return rc;
}
