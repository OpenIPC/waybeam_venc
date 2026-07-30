/* venc_jpeg.c — common JPEG snapshot logic shared across backends.
 *
 * Owns the public API surface (init/capture/shutdown), the module-wide
 * mutex, and the HTTP handler.  The per-backend file (star6e_jpeg.c or
 * maruko_jpeg.c) implements the actual SDK VENC channel lifecycle via
 * the venc_jpeg_backend_* hooks declared in include/venc_jpeg.h.
 */

#include "venc_jpeg.h"
#include "venc_httpd.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized;
static VencJpegConfig g_cfg;

int venc_jpeg_init(const VencJpegConfig *cfg)
{
	if (!cfg)
		return -EINVAL;

	pthread_mutex_lock(&g_jpeg_mutex);
	if (g_initialized) {
		pthread_mutex_unlock(&g_jpeg_mutex);
		return 0;
	}

	g_cfg = *cfg;
	/* Clamp quality to a sane range; SDK MJPEG quality is roughly
	 * MinQfactor/MaxQfactor on Star6E and a quality int on Maruko. */
	if (g_cfg.quality == 0) g_cfg.quality = 80;
	if (g_cfg.quality > 99) g_cfg.quality = 99;
	if (g_cfg.channel <= 0) g_cfg.channel = 7;

	if (!g_cfg.enabled) {
		g_initialized = true;
		pthread_mutex_unlock(&g_jpeg_mutex);
		return 0;
	}

	int rc = venc_jpeg_backend_init(&g_cfg);
	if (rc != 0) {
		fprintf(stderr, "[jpeg] backend_init failed %d (snapshot endpoint disabled)\n", rc);
		g_cfg.enabled = false;  /* Mark disabled; capture will return -ENODEV */
	}
	g_initialized = true;
	pthread_mutex_unlock(&g_jpeg_mutex);
	return rc;
}

void venc_jpeg_shutdown(void)
{
	pthread_mutex_lock(&g_jpeg_mutex);
	if (g_initialized && g_cfg.enabled) {
		venc_jpeg_backend_shutdown();
	}
	g_initialized = false;
	g_cfg.enabled = false;
	pthread_mutex_unlock(&g_jpeg_mutex);
}

int venc_jpeg_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;

	pthread_mutex_lock(&g_jpeg_mutex);
	if (!g_initialized || !g_cfg.enabled) {
		pthread_mutex_unlock(&g_jpeg_mutex);
		return -ENODEV;
	}
	int rc = venc_jpeg_backend_capture(out_buf, out_len, timeout_ms);
	pthread_mutex_unlock(&g_jpeg_mutex);
	return rc;
}

void venc_jpeg_free(uint8_t *buf)
{
	free(buf);
}

int venc_jpeg_set_quality(uint32_t q)
{
	if (q == 0) q = 1;
	if (q > 99) q = 99;

	pthread_mutex_lock(&g_jpeg_mutex);
	if (!g_initialized || !g_cfg.enabled) {
		pthread_mutex_unlock(&g_jpeg_mutex);
		return -ENODEV;
	}
	int rc = venc_jpeg_backend_set_quality(q);
	if (rc == 0)
		g_cfg.quality = q;
	pthread_mutex_unlock(&g_jpeg_mutex);
	return rc;
}

int handle_snapshot_jpeg(int client_fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;

	uint8_t *buf = NULL;
	size_t   len = 0;
	int rc = venc_jpeg_capture(&buf, &len, 1500);
	if (rc == -ENODEV) {
		return httpd_send_error(client_fd, 503, "snapshot_disabled",
			"snapshot endpoint not available (subsystem disabled or "
			"pipeline not running)");
	}
	if (rc == -ETIMEDOUT) {
		return httpd_send_error(client_fd, 504, "snapshot_timeout",
			"timed out waiting for a JPEG frame from the MJPEG channel");
	}
	if (rc != 0 || !buf || len == 0) {
		venc_jpeg_free(buf);
		return httpd_send_error(client_fd, 500, "snapshot_failed",
			"backend MJPEG capture failed");
	}

	int sent = httpd_send_binary(client_fd, 200, "image/jpeg", buf, (int)len);
	venc_jpeg_free(buf);
	return sent;
}

/* Default fallback for builds that don't link a backend (e.g. host-native
 * test runner).  Per-backend files override these with strong symbols.  */
__attribute__((weak)) void venc_jpeg_set_source(const void *vpe_port_opaque)
{
	(void)vpe_port_opaque;
}

__attribute__((weak)) int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	(void)cfg;
	return -ENOSYS;
}

__attribute__((weak)) int venc_jpeg_backend_capture(uint8_t **out_buf,
	size_t *out_len, uint32_t timeout_ms)
{
	(void)out_buf; (void)out_len; (void)timeout_ms;
	return -ENOSYS;
}

__attribute__((weak)) void venc_jpeg_backend_shutdown(void) { }

__attribute__((weak)) int venc_jpeg_backend_set_quality(uint32_t q)
{
	(void)q;
	return -ENOSYS;
}
