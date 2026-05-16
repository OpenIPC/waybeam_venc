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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized;
static VencJpegConfig g_cfg;

/* Concurrency / rate gates for the HTTP snapshot endpoint.
 *
 * Capture holds g_jpeg_mutex for up to the SDK timeout (1.5 s on
 * Star6E).  A parallel-curl DoS would otherwise pin the JPEG channel
 * and starve set_quality / shutdown.  Two cheap gates protect this:
 *
 *   1. g_capture_in_flight — atomic test-and-set; reject overlapping
 *      requests with 429 instead of queuing behind the mutex.
 *   2. g_last_capture_ms   — minimum 250 ms inter-request interval;
 *      reject bursts with 429 before they reach the SDK.
 *
 * Neither gate touches set_quality / init / shutdown paths. */
static atomic_int g_capture_in_flight;
static atomic_llong g_last_capture_ms;

#define SNAPSHOT_MIN_INTERVAL_MS 250

static long long monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

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

	/* Reject overlapping requests before the SDK is touched. */
	int expected = 0;
	if (!atomic_compare_exchange_strong(&g_capture_in_flight, &expected, 1)) {
		return httpd_send_error(client_fd, 429, "snapshot_busy",
			"another snapshot capture is already in flight");
	}

	/* Enforce a minimum inter-request interval.  Read-then-store is
	 * coarse (no CAS) but only the snapshot endpoint writes this
	 * value, and the in-flight gate above already serialises us. */
	long long now = monotonic_ms();
	long long last = atomic_load(&g_last_capture_ms);
	if (last != 0 && now - last < SNAPSHOT_MIN_INTERVAL_MS) {
		atomic_store(&g_capture_in_flight, 0);
		return httpd_send_error(client_fd, 429, "snapshot_rate_limited",
			"snapshot endpoint rate-limited "
			"(min 250 ms between requests)");
	}

	uint8_t *buf = NULL;
	size_t   len = 0;
	int rc = venc_jpeg_capture(&buf, &len, 1500);
	atomic_store(&g_last_capture_ms, monotonic_ms());
	atomic_store(&g_capture_in_flight, 0);

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
