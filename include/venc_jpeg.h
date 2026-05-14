#ifndef VENC_JPEG_H
#define VENC_JPEG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "venc_httpd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JPEG snapshot subsystem.
 *
 * Single, dedicated MJPEG VENC channel that taps the same VPE/SCL output
 * port the main H.264/H.265 channel already consumes.  Allocated once at
 * pipeline startup, kept idle (StartRecvPic off) between requests, and
 * pulse-encoded on demand by GET /api/v1/snapshot.jpg.
 *
 * Lifecycle:
 *   pipeline-start  → venc_jpeg_init(&cfg)
 *   per-request     → venc_jpeg_capture(&buf, &len, timeout_ms)
 *   pipeline-stop   → venc_jpeg_shutdown()
 *
 * Concurrency: venc_jpeg_capture() is serialized internally.  Multiple
 * HTTP clients hitting /snapshot.jpg simultaneously queue rather than
 * stomp the JPEG channel.
 *
 * Per-backend stubs live in src/star6e_jpeg.c and src/maruko_jpeg.c.
 * The common HTTP handler + locking lives in src/venc_jpeg.c.
 */

typedef struct {
	uint32_t width;       /* 0 = inherit from main stream */
	uint32_t height;      /* 0 = inherit from main stream */
	uint32_t quality;     /* 1–100, MJPEG q-factor; clamped */
	int      channel;     /* VENC channel ID (default 7; well clear of dual) */
	bool     enabled;     /* If false, init is a no-op, capture returns ENOENT */
} VencJpegConfig;

/* Initialize the JPEG subsystem.  Returns 0 on success.  Idempotent —
 * re-init returns 0 without touching SDK state if already initialized.
 *
 * Must be called after the main VPE/VENC pipeline is up so the backend
 * can bind to the active VPE output port.  Backend implementations grab
 * the VPE port info via star6e_jpeg_set_source() / maruko_jpeg_set_source()
 * which the pipeline calls during its own bring-up. */
int venc_jpeg_init(const VencJpegConfig *cfg);

/* Tear down the JPEG channel.  Idempotent.  Must be called before the
 * main VPE/VENC pipeline tears down its source ports. */
void venc_jpeg_shutdown(void);

/* Capture one JPEG frame.  Allocates *out_buf (caller frees via
 * venc_jpeg_free).  Returns 0 on success, -ENODEV if subsystem disabled,
 * -ETIMEDOUT if no frame within timeout_ms, -EIO on SDK failure.
 *
 * Internally serialized; safe to call from multiple HTTP worker threads. */
int venc_jpeg_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms);

/* Free a buffer returned by venc_jpeg_capture(). */
void venc_jpeg_free(uint8_t *buf);

/* Live-update MJPEG quality factor (1..99) on the running channel.
 * Internally serialized via the same mutex as venc_jpeg_capture, so the
 * SDK Set/Get sequence cannot interleave with a capture in progress.
 * Returns 0 on success, -ENODEV if the snapshot subsystem is disabled,
 * -ENOSYS if the backend has no set-quality hook, -EIO on SDK failure. */
int venc_jpeg_set_quality(uint32_t q);

/* HTTP handler for GET /api/v1/snapshot.jpg.  Returns image/jpeg on
 * success, application/json {ok:false,error:{...}} on failure. */
int handle_snapshot_jpeg(int client_fd, const HttpRequest *req, void *ctx);

/* ── Backend interface (implemented per-SOC) ─────────────────────────── */

/* Backend-private: register VPE/SCL output port info with the JPEG
 * module.  The pipeline calls this during init right after the main
 * VPE port is configured.  After this call, venc_jpeg_init() can bind
 * the MJPEG channel to the same source. */
struct MI_SYS_ChnPort_t_;
void venc_jpeg_set_source(const void *vpe_port_opaque);

/* Backend-private: create + bind MJPEG channel.  Channel stays idle
 * (StartRecvPic off) after this returns.  Called from venc_jpeg_init. */
int venc_jpeg_backend_init(const VencJpegConfig *cfg);

/* Backend-private: capture one JPEG.  Called from venc_jpeg_capture
 * under the module lock.  Implementation does StartRecvPic → wait for
 * frame → GetStream → memcpy → ReleaseStream → StopRecvPic. */
int venc_jpeg_backend_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms);

/* Backend-private: destroy MJPEG channel.  Called from venc_jpeg_shutdown. */
void venc_jpeg_backend_shutdown(void);

/* Backend-private: Get→modify→Set MJPEG channel quality on the running
 * channel.  Called from venc_jpeg_set_quality under the module lock.
 * Returns 0 on success, -EIO on SDK error, -ENOSYS when the backend
 * does not implement live quality (host-test fallback). */
int venc_jpeg_backend_set_quality(uint32_t q);

#ifdef __cplusplus
}
#endif

#endif /* VENC_JPEG_H */
