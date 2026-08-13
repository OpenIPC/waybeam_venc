#ifndef QR_SCAN_H
#define QR_SCAN_H

#include <stddef.h>
#include <stdint.h>

/*
 * qr_scan — the bounded QR decode cascade used by the freestanding
 * tools/qr/qr_decode helper. The venc daemon invokes that helper rather than
 * linking the vendored decoder into its long-lived process.
 *
 * Keeping the cascade in one helper prevents CLI and on-device scan behavior
 * from drifting. Its stage order, thresholds and outer-frame gating are
 * measured against the corpus in
 * tools/qr/test-images plus the generated perspective series in
 * tests/test_qr_marker.c; a second copy would drift silently and the drift
 * would only show up as a craft that stops pairing.
 *
 * The caller supplies a tightly packed 8-bit grayscale image (no stride).  The
 * CLI gets one from a JPEG or P5 PGM; the daemon gets one from the VPE port1
 * luma tap.  Image loading deliberately stays OUT of here: it is the only part
 * that needs stb_image, and linking a JPEG decoder into the daemon to feed it
 * pixels it already has in DRAM would be absurd.
 *
 * Memory: peak heap is ~3x W*H plus quirc's fixed region table.  The cascade
 * holds the caller's image, one W*H scratch arena, and quirc's own W*H image
 * (which aliases its pixel buffer at QUIRC_MAX_REGIONS=254 and retains a
 * high-water allocation across passes).  At the Star6E tap's 1080x1080 that is
 * ~3.5 MB against ~44 MB MemAvailable.
 */

/* Matches QUIRC_MAX_PAYLOAD.  Asserted against it in qr_scan.c so a quirc
 * bump cannot silently truncate. */
#define QR_SCAN_PAYLOAD_MAX 8896

typedef struct {
	int      enabled;          /* emit per-stage tracing to stderr        */
	int      fatal_error;      /* allocation failure — result is unusable */
	int      aborted;          /* abort callback asked us to stop         */
	uint64_t prepare_us;
	uint64_t transform_us;
	uint64_t identify_us;
	uint64_t decode_us;
	unsigned regions;
	unsigned frame_candidates;
	unsigned refinements;
	unsigned finder_candidates;
	unsigned lens_corrections;
	unsigned qr_candidates;
	unsigned mirror_attempts;
	unsigned qr_decoded;
	unsigned envelope_rejected;
	char     success_stage[32];
} QrScanStats;

typedef struct {
	uint8_t  payload[QR_SCAN_PAYLOAD_MAX];
	unsigned payload_len;
} QrScanResult;

typedef struct QrScanCtx QrScanCtx;

/* Allocate a scan context (wraps one struct quirc).  Reuse it across attempts:
 * quirc retains its image allocation at the high-water size, so a fresh context
 * per attempt would re-malloc W*H every time.  Returns NULL on failure. */
QrScanCtx *qr_scan_ctx_new(void);
void       qr_scan_ctx_free(QrScanCtx *ctx);

/* Install an abort callback, checked at the top of every region and refinement
 * pass.  Returning non-zero makes the cascade unwind promptly with
 * stats->aborted set and no result.  Pass NULL to clear.
 *
 * This is what lets a scan window honour its deadline and answer /qr/stop
 * without waiting out a full ~1.5 s cascade.  Without it the only granularity
 * available is "one whole attempt". */
void qr_scan_set_abort(QrScanCtx *ctx, int (*cb)(void *user), void *user);

/* Run the cascade over a tightly packed w*h grayscale image.
 *
 * `raw` skips the Waybeam transport-envelope check (bench diagnostics only —
 * it accepts any QR found inside the required outer frame).
 *
 * Returns 1 when a payload was accepted and written to *out, 0 otherwise.
 * `out` and `stats` must both be non-NULL; zero them or reuse a zeroed struct.
 * Check stats->fatal_error to distinguish "no code here" from "we ran out of
 * memory and never really looked". */
int qr_scan_image(QrScanCtx *ctx, const uint8_t *pix, int w, int h, int raw,
	QrScanResult *out, QrScanStats *stats);

/* 3x3 box blur, in place, edges copied through.
 *
 * Exposed only so tests can assert it is bit-identical to the allocating
 * reference implementation.  It rewrites the buffer under a two-row ring of
 * saved originals, which is what removes one full W*H allocation from the
 * cascade's peak; a subtle bug here would degrade decode rate rather than
 * crash, so it is pinned by test rather than by inspection.
 *
 * Returns 0 on success, -1 if the two-row scratch could not be allocated. */
int qr_scan_box_blur3_inplace(uint8_t *buf, int w, int h);

#endif /* QR_SCAN_H */
