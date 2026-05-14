/* test_venc_jpeg.c — unit tests for src/venc_jpeg.c
 *
 * Covers the common (HTTP/lock) layer.  Backend MJPEG plumbing is
 * hardware-dependent and validated on-device via
 * scripts/maruko_sensor_init_diff.sh.
 *
 * The host test_runner links src/venc_jpeg.c with no backend file,
 * so the weak fallback stubs in venc_jpeg.c resolve every backend
 * symbol — meaning every code path here exercises the public API in
 * the "no backend present" state (init fails → endpoint disabled).
 */

#include "test_helpers.h"
#include "venc_jpeg.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int test_venc_jpeg(void)
{
	int failures = 0;

	/* Reset to a clean state in case a previous test suite left the
	 * static `g_initialized` flag set. */
	venc_jpeg_shutdown();

	/* Capture without init must refuse. */
	{
		uint8_t *buf = NULL;
		size_t   len = 0;
		int rc = venc_jpeg_capture(&buf, &len, 100);
		CHECK("capture before init returns -ENODEV", rc == -ENODEV);
		CHECK("capture before init leaves buf NULL", buf == NULL);
		CHECK("capture before init leaves len 0",    len == 0);
	}

	/* NULL args rejected with -EINVAL. */
	{
		size_t len = 0;
		CHECK("capture rejects NULL buf",
			venc_jpeg_capture(NULL, &len, 0) == -EINVAL);
		uint8_t *buf = NULL;
		CHECK("capture rejects NULL len",
			venc_jpeg_capture(&buf, NULL, 0) == -EINVAL);
	}

	/* Init with NULL cfg rejected. */
	CHECK("init rejects NULL cfg", venc_jpeg_init(NULL) == -EINVAL);

	/* Init with enabled=false is a no-op success.  Subsequent capture
	 * returns -ENODEV because the subsystem is marked disabled. */
	{
		VencJpegConfig cfg = {
			.width = 1920, .height = 1080, .quality = 80,
			.channel = 7, .enabled = false,
		};
		CHECK("init(enabled=false) returns 0", venc_jpeg_init(&cfg) == 0);

		uint8_t *buf = NULL;
		size_t   len = 0;
		CHECK("capture after init(enabled=false) returns -ENODEV",
			venc_jpeg_capture(&buf, &len, 100) == -ENODEV);
	}

	venc_jpeg_shutdown();

	/* Init with enabled=true, no backend linked → backend_init returns
	 * -ENOSYS via the weak stub; module marks itself disabled, so
	 * capture still returns -ENODEV (clean degradation). */
	{
		VencJpegConfig cfg = {
			.width = 1920, .height = 1080, .quality = 80,
			.channel = 7, .enabled = true,
		};
		int rc = venc_jpeg_init(&cfg);
		CHECK("init(enabled=true,no backend) returns backend err",
			rc == -ENOSYS);

		uint8_t *buf = NULL;
		size_t   len = 0;
		CHECK("capture after failed backend init still -ENODEV",
			venc_jpeg_capture(&buf, &len, 100) == -ENODEV);
	}

	venc_jpeg_shutdown();

	/* venc_jpeg_free with NULL is a no-op (matches free(NULL)). */
	venc_jpeg_free(NULL);
	CHECK("free(NULL) does not crash", 1);

	/* Shutdown is idempotent. */
	venc_jpeg_shutdown();
	venc_jpeg_shutdown();
	CHECK("double shutdown does not crash", 1);

	/* Re-init after shutdown should work again. */
	{
		VencJpegConfig cfg = {
			.width = 1280, .height = 720, .quality = 80,
			.channel = 7, .enabled = false,
		};
		CHECK("re-init after shutdown returns 0",
			venc_jpeg_init(&cfg) == 0);
	}

	venc_jpeg_shutdown();
	return failures;
}
