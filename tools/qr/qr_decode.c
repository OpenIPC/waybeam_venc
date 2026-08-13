/* qr_decode.c — decode a QR code from a JPEG or binary P5 PGM image.
 *
 * Reads the image from a file argument (or stdin when the argument is "-" or
 * absent), sniffing the format from the first two bytes: JPEG (FF D8) is
 * decoded to its luma plane via the vendored stb_image, P5 PGM is parsed
 * directly.  Then finds a continuous outer-frame marker and prints the payload
 * of the first successfully decoded QR code which passes the minimal Waybeam
 * transport-envelope checks. --raw disables those checks for diagnostics.
 *
 * Robustness for real captures (perspective, small codes in a large frame,
 * rotation, mirroring):
 *   - required continuous outer-frame geometry supplies a direct projective
 *     transform for Version-1 symbols, without a rectified image;
 *   - each detected code is retried through quirc_flip() so mirrored/flipped
 *     codes decode (ISO 18004:2015 mirror feature);
 *   - measured low-cost full-frame and half-scale passes run first; radial
 *     correction handles strong fisheye curvature, and overlapping tiles stay
 *     late as the expensive small-code fallback.
 *   The first success wins.
 *
 * Exit codes:
 *   0  a QR code was decoded; payload written to stdout
 *   1  no QR code found / not decodable
 *   2  usage or input error (bad image, allocation failure)
 *
 * Pairs with waybeam's GET /api/v1/snapshot.jpg endpoint:
 *   curl -s http://127.0.0.1/api/v1/snapshot.jpg | qr_decode
 *
 * This file is the CLI shell only: option parsing, image loading and output.
 * The decode cascade itself lives in src/qr_scan.c and is shared verbatim with
 * the venc daemon's scan-window worker, so the two can never drift.
 *
 * quirc (ISC) is vendored under tools/qr/quirc/; stb_image (public domain)
 * under tools/qr/stb/, compiled JPEG-only in this TU.
 */

#include "qr_scan.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Vendored stb_image, JPEG decode only.  Never edit the vendored file; any
 * build-side accommodation (like the unused-function silence for the parts
 * the JPEG-only build never calls) lives here. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define MAX_INPUT_DIM 4096u

struct decode_options {
	const char *path;
	int raw;
	int stats;
};

/* CLI-only timing that brackets the shared cascade. */
struct decode_stats {
	int enabled;
	uint64_t started_us;
	uint64_t load_us;
};

static void usage(FILE *f)
{
	fprintf(f,
		"usage: qr_decode [--raw] [--stats] [--] [FILE|-]\n"
		"\n"
		"Default: decode the first valid Waybeam Version-1/Q 16-character\n"
		"envelope inside the required continuous outer-frame profile.\n"
		"--raw emits any framed QR payload for bench diagnostics.\n"
		"--stats writes per-stage timings and decode status to stderr.\n");
}

static int parse_options(int argc, char **argv, struct decode_options *opts)
{
	int input_seen = 0;
	int options_done = 0;
	int i;

	memset(opts, 0, sizeof(*opts));

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!options_done && strcmp(arg, "--raw") == 0) {
			opts->raw = 1;
		} else if (!options_done && strcmp(arg, "--stats") == 0) {
			opts->stats = 1;
		} else if (!options_done && (strcmp(arg, "-h") == 0 ||
					    strcmp(arg, "--help") == 0)) {
			usage(stdout);
			return 1;
		} else if (!options_done && strcmp(arg, "--") == 0) {
			options_done = 1;
		} else if (strcmp(arg, "-") == 0 || options_done || arg[0] != '-') {
			if (input_seen) {
				fprintf(stderr, "qr_decode: only one input is allowed\n");
				return -1;
			}
			input_seen = 1;
			opts->path = strcmp(arg, "-") == 0 ? NULL : arg;
		} else {
			fprintf(stderr, "qr_decode: unknown option: %s\n", arg);
			return -1;
		}
	}
	return 0;
}

static uint64_t monotonic_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000u +
	       (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t stats_now(const struct decode_stats *stats)
{
	return stats->enabled ? monotonic_us() : 0;
}

/* Read the whole stream into a malloc'd buffer (needed for JPEG, which stb
 * decodes from memory, and lets the format sniff work on stdin too).
 * Bounded so a runaway pipe cannot exhaust RAM.  Returns the buffer (caller
 * frees) and writes *len, or NULL on error. */
#define MAX_INPUT_BYTES (64u * 1024u * 1024u)
static uint8_t *slurp(FILE *f, size_t *len)
{
	size_t cap = 1u << 20, used = 0;
	uint8_t *buf = malloc(cap);

	if (!buf) {
		fprintf(stderr, "qr_decode: out of memory\n");
		return NULL;
	}
	for (;;) {
		if (used == cap) {
			if (cap >= MAX_INPUT_BYTES) {
				fprintf(stderr, "qr_decode: input exceeds %u "
					"bytes\n", MAX_INPUT_BYTES);
				free(buf);
				return NULL;
			}
			cap *= 2;
			uint8_t *nb = realloc(buf, cap);
			if (!nb) {
				fprintf(stderr, "qr_decode: out of memory\n");
				free(buf);
				return NULL;
			}
			buf = nb;
		}
		size_t n = fread(buf + used, 1, cap - used, f);
		used += n;
		if (n == 0) {
			if (ferror(f)) {
				fprintf(stderr, "qr_decode: read error\n");
				free(buf);
				return NULL;
			}
			break;   /* EOF */
		}
	}
	*len = used;
	return buf;
}

/* Read the next unsigned integer from a P5 header in memory, skipping
 * whitespace and '#' comment lines.  Advances *pos.  Returns 0 on success,
 * -1 on end-of-buffer/format error. */
static int pgm_read_uint(const uint8_t *buf, size_t len, size_t *pos,
	unsigned *out)
{
	int c;

	for (;;) {
		if (*pos >= len)
			return -1;
		c = buf[(*pos)++];
		if (c == '#') {
			while (*pos < len && buf[*pos] != '\n')
				(*pos)++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			continue;
		break;
	}
	if (c < '0' || c > '9')
		return -1;

	unsigned v = 0;
	while (c >= '0' && c <= '9') {
		unsigned digit = (unsigned)(c - '0');

		if (v > (UINT_MAX - digit) / 10u)
			return -1;
		v = v * 10u + digit;
		if (*pos >= len)
			return -1;
		c = buf[(*pos)++];
	}
	if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
		return -1;
	*out = v;
	return 0;
}

/* Parse a binary P5 PGM from memory into a freshly malloc'd 8-bit grayscale
 * buffer.  Same validations as the historical stream parser. */
static uint8_t *pgm_from_mem(const uint8_t *buf, size_t len,
	unsigned *w, unsigned *h)
{
	size_t pos = 2;   /* past the sniffed "P5" magic */
	unsigned maxval = 0;

	if (pgm_read_uint(buf, len, &pos, w) != 0 ||
	    pgm_read_uint(buf, len, &pos, h) != 0 ||
	    pgm_read_uint(buf, len, &pos, &maxval) != 0) {
		fprintf(stderr, "qr_decode: malformed PGM header\n");
		return NULL;
	}
	if (*w == 0 || *h == 0 || *w > MAX_INPUT_DIM ||
	    *h > MAX_INPUT_DIM || maxval != 255) {
		fprintf(stderr, "qr_decode: unsupported PGM (w=%u h=%u max=%u; "
			"need dimensions 1..%u and maxval 255)\n",
			*w, *h, maxval, MAX_INPUT_DIM);
		return NULL;
	}
	/* Exactly one whitespace byte separates the header from the raster;
	 * pgm_read_uint already consumed it while scanning past maxval. */

	size_t npix = (size_t)*w * *h;
	if (len - pos < npix) {
		fprintf(stderr, "qr_decode: short pixel data\n");
		return NULL;
	}
	uint8_t *pix = malloc(npix);
	if (!pix) {
		fprintf(stderr, "qr_decode: out of memory\n");
		return NULL;
	}
	memcpy(pix, buf + pos, npix);
	return pix;
}

/* Decode a JPEG from memory to its luma plane via stb_image. */
static uint8_t *jpeg_from_mem(const uint8_t *buf, size_t len,
	unsigned *w, unsigned *h)
{
	int iw = 0, ih = 0, comp = 0;
	uint8_t *pix;

	if (len > INT_MAX) {
		fprintf(stderr, "qr_decode: JPEG too large\n");
		return NULL;
	}
	pix = stbi_load_from_memory(buf, (int)len, &iw, &ih, &comp, 1);
	if (!pix) {
		fprintf(stderr, "qr_decode: JPEG decode failed: %s\n",
			stbi_failure_reason());
		return NULL;
	}
	if (iw <= 0 || ih <= 0 || (unsigned)iw > MAX_INPUT_DIM ||
	    (unsigned)ih > MAX_INPUT_DIM) {
		fprintf(stderr, "qr_decode: unsupported JPEG (w=%d h=%d; "
			"need dimensions 1..%u)\n", iw, ih, MAX_INPUT_DIM);
		stbi_image_free(pix);
		return NULL;
	}
	*w = (unsigned)iw;
	*h = (unsigned)ih;
	/* stbi buffers come from malloc, so the caller's free() is fine. */
	return pix;
}

/* Load a JPEG or binary P5 PGM into a freshly malloc'd 8-bit grayscale
 * buffer, sniffing the format from the first two bytes.  Returns the pixel
 * buffer (caller frees) and writes w/h, or NULL on error. */
static uint8_t *image_load(FILE *f, unsigned *w, unsigned *h)
{
	size_t len = 0;
	uint8_t *buf = slurp(f, &len);
	uint8_t *pix = NULL;

	if (!buf)
		return NULL;
	if (len >= 2 && buf[0] == 'P' && buf[1] == '5')
		pix = pgm_from_mem(buf, len, w, h);
	else if (len >= 2 && buf[0] == 0xFF && buf[1] == 0xD8)
		pix = jpeg_from_mem(buf, len, w, h);
	else
		fprintf(stderr, "qr_decode: unrecognized input (need JPEG or "
			"binary P5 PGM)\n");
	free(buf);
	return pix;
}

int main(int argc, char **argv)
{
	struct decode_options opts;
	struct decode_stats cli;
	QrScanStats stats;
	QrScanResult result;
	QrScanCtx *ctx;
	int opt_rc = parse_options(argc, argv, &opts);
	if (opt_rc > 0)
		return 0;
	if (opt_rc < 0) {
		usage(stderr);
		return 2;
	}

	memset(&cli, 0, sizeof(cli));
	memset(&stats, 0, sizeof(stats));
	memset(&result, 0, sizeof(result));
	cli.enabled = opts.stats;
	stats.enabled = opts.stats;
	cli.started_us = stats_now(&cli);
	const char *path = opts.path;
	FILE *f = path ? fopen(path, "rb") : stdin;
	if (!f) {
		fprintf(stderr, "qr_decode: cannot open %s\n", path);
		return 2;
	}

	unsigned w = 0, h = 0;
	uint64_t load_started = stats_now(&cli);
	uint8_t *pix = image_load(f, &w, &h);
	cli.load_us = stats_now(&cli) - load_started;
	if (path)
		fclose(f);
	if (!pix)
		return 2;
	if (cli.enabled)
		fprintf(stderr,
			"[qr-decode] input=%s size=%ux%u "
			"profile=outer-frame-v1\n",
			path ? path : "stdin", w, h);

	ctx = qr_scan_ctx_new();
	if (!ctx) {
		fprintf(stderr, "qr_decode: qr_scan_ctx_new failed\n");
		free(pix);
		return 2;
	}
	int decoded = qr_scan_image(ctx, pix, (int)w, (int)h, opts.raw,
				    &result, &stats);
	int rc = stats.fatal_error ? 2 : (decoded ? 0 : 1);

	if (decoded) {
		fwrite(result.payload, 1, result.payload_len, stdout);
		fputc('\n', stdout);
	}

	qr_scan_ctx_free(ctx);
	free(pix);
	if (cli.enabled)
		fprintf(stderr,
			"[qr-decode] summary result=%s final_stage=%s "
			"total_us=%llu load_us=%llu prepare_us=%llu "
			"transform_us=%llu identify_us=%llu decode_us=%llu "
			"regions=%u frames=%u refinements=%u finders=%u "
			"lens_corrections=%u "
			"qr_candidates=%u "
			"mirror_attempts=%u qr_decoded=%u "
			"envelope_rejected=%u\n",
			rc == 0 ? "decoded" :
			(rc == 1 ? "no-decode" : "fatal-error"),
			stats.success_stage[0] ? stats.success_stage : "none",
			(unsigned long long)(monotonic_us() -
					     cli.started_us),
			(unsigned long long)cli.load_us,
			(unsigned long long)stats.prepare_us,
			(unsigned long long)stats.transform_us,
			(unsigned long long)stats.identify_us,
			(unsigned long long)stats.decode_us,
			stats.regions, stats.frame_candidates,
			stats.refinements, stats.finder_candidates,
			stats.lens_corrections,
			stats.qr_candidates, stats.mirror_attempts,
			stats.qr_decoded, stats.envelope_rejected);
	return rc;
}
