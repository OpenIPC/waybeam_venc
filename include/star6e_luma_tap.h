#ifndef STAR6E_LUMA_TAP_H
#define STAR6E_LUMA_TAP_H

#include <stddef.h>
#include <stdint.h>

#include "venc_config.h"

typedef struct Star6eLumaTap Star6eLumaTap;

Star6eLumaTap *star6e_luma_tap_create(void);
void star6e_luma_tap_destroy(Star6eLumaTap *tap);

/*
 * Read-only NV12 luma tap on VPE port1 (Star6E).
 *
 * Exists so a vision consumer (QR scanning) can read frames that are NOT
 * composited with any MI_RGN overlay.  Overlays attach per scaler output port
 * and both debug_osd and waybeam-hub's osd_render target port0, so port0 — and
 * therefore the MJPEG snapshot channel that 1:N-binds to it — carries HUD
 * pixels over anything a marker might occupy.  port1 is a separate scaler
 * output and is clean.
 *
 * Lifecycle: the port is claimed and enabled for the duration of a SCAN WINDOW
 * and released at its end.  Bring-up only arms the tap.  Holding it for the
 * whole pipeline run was measured too expensive — ~186 MB/s of SCL write
 * traffic at 1080p60 and 8-9 points of CPU while idle — and it locks detect and
 * stab out of port1 for nothing when nobody is scanning.
 *
 * Both edges of that cycle are floored, and this is load-bearing.  The retired
 * /api/v1/snapshot.pgm enabled and disabled port1 per HTTP request; DisablePort
 * racing an in-flight mhal buffer wedges the VPE input FIFO, kernel-side.  Two
 * bench boxes took hard panics from /qr/scan followed immediately by /qr/stop,
 * which closes a port that only just came up.  So LT_REOPEN_COOLDOWN_MS floors
 * the gap between opens and LT_MIN_WINDOW_MS floors how long the port stays up,
 * enforced inside lt_port_close() where no path can bypass it.
 *
 * Threading: the reader thread drains EVERY frame at line rate and copies the
 * luma plane out only when a grab is pending, so a slow consumer can never sit
 * between GetBuf and PutBuf.  A supervisor thread — the ONLY thread that opens
 * or closes the port — invokes the isolated decoder helper and self-closes
 * the window.
 * grab() is called from HTTP worker threads and is internally serialized.
 */

/* Remember the tap settings without touching the port.  Called at pipeline
 * bring-up so a later open() knows its geometry.  The port is NOT claimed and
 * NOT enabled here: an always-on tap costs ~186 MB/s of SCL write bandwidth at
 * 1080p60 and holds port1 against detect/stab for the whole run, for nothing
 * when no scan is in progress. */
void star6e_luma_tap_configure(Star6eLumaTap *tap, const VencConfig *cfg,
	uint32_t main_w, uint32_t main_h);

/* Start a scan window of `window_ms` (0 = the configured qr.windowMs), or
 * EXTEND the one already running.  Extending never touches port state — it only
 * moves the deadline — so repeated scan requests are cheap and cannot
 * re-introduce the Enable/Disable cycling that wedged snapshot.pgm.
 *
 * Returns 0, -EBUSY when port1 is held by stab/detect (reported immediately,
 * never queued), -ENODEV when unarmed, -EIO when the SDK refuses the geometry
 * or the enabled port delivers no frame.
 *
 * May block for up to LT_REOPEN_COOLDOWN_MS enforcing the gap between opens.
 *
 * Safe from any thread.  Opening happens on the CALLER's thread, deliberately,
 * so the caller gets the real error instead of a window that dies silently
 * moments later; ctl_lock serializes it against the supervisor, which owns
 * every close.  So no SDK port call is ever concurrent. */
int star6e_luma_tap_scan(Star6eLumaTap *tap, uint32_t window_ms);

/* End the current window early.  Blocks until the supervisor has closed the
 * port and released port1, so a caller can rely on port1 being free on return —
 * which includes waiting out LT_MIN_WINDOW_MS when the window is brand new.
 * Idempotent. */
void star6e_luma_tap_scan_stop(Star6eLumaTap *tap);

/* Payload is a Waybeam transport envelope: exactly 16 characters from the QR
 * alphanumeric alphabet (0-9 A-Z and " $%*+-./:").  Nothing in that set needs
 * JSON escaping, which is why /api/v1/qr/status can embed it directly. */
#define STAR6E_QR_PAYLOAD_MAX 32

typedef struct {
	int      armed;        /* qr.tap_enabled and pipeline up            */
	int      scanning;     /* a window is open                          */
	uint32_t width, height;/* latch (centre-square) geometry            */
	uint32_t window_ms;    /* budget of the current/last window         */
	int64_t  remaining_ms; /* <=0 when not scanning                     */
	uint64_t frames;       /* frames drained this window                */
	uint64_t grabs;        /* frames actually copied out this window    */
	char     port1_owner[24]; /* "" when free                           */

	/* Decode result.  Retained after the window closes and cleared only by
	 * the next scan, so a client that polls once per second still sees the
	 * payload from a window that ended between polls. */
	uint32_t attempts;     /* helper invocations this window           */
	int      decoded;      /* a payload was accepted                    */
	char     payload[STAR6E_QR_PAYLOAD_MAX];
	char     stage[32];    /* "qr_decode" when the helper succeeds      */
	uint64_t decode_us;    /* duration of the winning helper invocation */
	uint64_t last_us;      /* duration of the most recent invocation    */
} Star6eLumaTapStatus;

/* Snapshot the tap state for /api/v1/qr/status.  Safe from any thread. */
void star6e_luma_tap_status(Star6eLumaTap *tap, Star6eLumaTapStatus *out);

/* Pipeline teardown: end any open window and disarm, so a later scan cannot
 * resurrect the tap against a torn-down graph.  Blocks until port1 is free.
 * Idempotent; safe when the tap was never armed.  Must run BEFORE the VPE
 * source ports are unbound. */
void star6e_luma_tap_stop(Star6eLumaTap *tap);

/* Grab one frame's luma plane as a freshly malloc'd P5 PGM blob (tightly
 * packed, stride removed, self-describing dimensions).  Caller frees with
 * star6e_luma_tap_free().
 *
 * Returns 0 on success, -ENODEV when no window is open, -EBUSY while the
 * decoder helper owns the latch, -ETIMEDOUT when no frame arrives within timeout_ms,
 * -EIO on a malformed buffer, -ENOMEM on allocation failure. */
int star6e_luma_tap_grab_pgm(Star6eLumaTap *tap, uint8_t **out_buf,
	size_t *out_len, uint32_t timeout_ms);

/* Free a buffer returned by star6e_luma_tap_grab_pgm(). */
void star6e_luma_tap_free(uint8_t *buf);

/* True while the tap is running, for endpoint gating. */
int star6e_luma_tap_running(const Star6eLumaTap *tap);

#endif /* STAR6E_LUMA_TAP_H */
