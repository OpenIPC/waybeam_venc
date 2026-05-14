/* maruko_jpeg.c — Maruko (Infinity6C) MJPEG snapshot backend.
 *
 * STATUS: deferred — returns -ENOSYS so /api/v1/snapshot.jpg serves a
 * clean 503 on Maruko.  Star6E is fully supported (see src/star6e_jpeg.c).
 *
 * Bench investigation on 192.168.2.12 (v0.10.9, 2026-05-14):
 *
 *   Attempt 1 — bind SCL output → MJPG VENC dev 8:
 *     MI_SYS_BindChnPort2 → 0xA0092012 ("SYS busy").  Same error code
 *     the dual-stream path documents at maruko_pipeline.c:2097 — the
 *     SCL output port is 1:1, and the main H.265 channel already
 *     holds it in I6_SYS_LINK_RING mode.  Cannot multi-consume.
 *
 *   Attempt 2 — bind main VENC chn 0 output → MJPG VENC dev 8 chn 0
 *               (HW_RING fan-out across devices, mirroring the dual-
 *               stream sub-channel pattern):
 *     CreateDev/CreateChn appear to succeed but the failure path on
 *     bind left a [venc8_P0_MAIN] kernel thread alive.  When the
 *     subsequent run tried to MI_SYS_Init, the orphaned kthread
 *     blocked it indefinitely (httpd up, pipeline never starts —
 *     same teardown bug documented in HISTORY.md "venc_teardown
 *     regression").  Required sysrq-b to recover the bench.
 *
 * Two viable paths forward (out of scope for this PR):
 *   a) Add a second SCL output port (port 1) at pipeline init, bind
 *      it to the MJPG channel — requires SCL setup changes in
 *      src/maruko_pipeline.c.
 *   b) Re-test cross-device VENC HW_RING fan-out with safer init
 *      teardown (e.g. probe BindChnPort2 BEFORE CreateChn so a
 *      failure path doesn't leave a kernel thread behind).
 *
 * Both require iteration on hardware that's intentionally not part of
 * this PR's scope.  Until then we keep the endpoint registered so
 * client code can be written against it; capture returns -ENOSYS →
 * 503 "snapshot_disabled" cleanly.
 */

#include "venc_jpeg.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

static int g_announced = 0;

void venc_jpeg_set_source(const void *port_opaque)
{
	(void)port_opaque;
}

int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	(void)cfg;
	if (!g_announced) {
		fprintf(stderr, "[jpeg-maruko] backend deferred — snapshot disabled "
			"on this build (see src/maruko_jpeg.c header)\n");
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
