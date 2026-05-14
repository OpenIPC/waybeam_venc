/* maruko_jpeg.c — Maruko (Infinity6C) MJPEG snapshot backend.
 *
 * Phase 3 stub.  Returns -ENOSYS so the endpoint serves a clean 503
 * "snapshot_disabled" until the real implementation lands.  See
 * star6e_jpeg.c for the reference pattern; Maruko mirrors it through
 * the maruko_mi_venc_* dlopen dispatch table (include/maruko_mi.h).
 */

#include "venc_jpeg.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

static int g_announced = 0;

void venc_jpeg_set_source(const void *vpe_port_opaque)
{
	(void)vpe_port_opaque;
}

int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	(void)cfg;
	if (!g_announced) {
		fprintf(stderr, "[jpeg-maruko] backend stub — snapshot disabled "
			"on this build (Phase 3 TODO)\n");
		g_announced = 1;
	}
	return -ENOSYS;
}

int venc_jpeg_backend_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	(void)out_buf; (void)out_len; (void)timeout_ms;
	return -ENOSYS;
}

void venc_jpeg_backend_shutdown(void)
{
}
