/* Minimal frame-SHM ring consumer / validator for the frame-shm:// output.
 * Attaches to a venc_frame_ring, reads whole encoded frames, decodes the
 * 8-byte VencFrameMeta header, and reports frame rate, IDR cadence, frame
 * sizes, pts monotonicity, and Annex-B start-code integrity.
 *
 * Usage: frame_shm_consumer_test [ring_name] [duration_s]
 *   ring_name  default "venc_frames"
 *   duration_s default 5 (0 = run until SIGINT)
 *
 * Cross-compile for Star6E (Infinity6E):
 *   toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-gcc \
 *     -Os -Iinclude -D_GNU_SOURCE \
 *     tools/frame_shm_consumer_test.c src/venc_frame_ring.c -lpthread \
 *     -o frame_shm_consumer
 *
 * Reports frame rate, IDR cadence, frame sizes, pts monotonicity, and
 * Annex-B start-code integrity. Exit 0 = PASS, 2 = FAIL. Attaches as a
 * pure consumer (never unlinks the ring); safe to kill/restart at will.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>

#include "venc_frame_ring.h"

static volatile int running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

/* 512 KB slot -> a frame can be up to slot_data_size; size buf generously. */
#define BUF_SIZE (512 * 1024)

int main(int argc, char **argv)
{
	const char *name = (argc > 1) ? argv[1] : "venc_frames";
	int duration = (argc > 2) ? atoi(argv[2]) : 5;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	venc_frame_ring_t *r = venc_frame_ring_attach(name);
	if (!r) {
		fprintf(stderr, "Failed to attach to frame ring '%s'\n", name);
		return 1;
	}

	printf("Attached to frame ring '%s': %u slots x %u KB (epoch=%u, V%u, magic=0x%08X)\n",
	       name, r->hdr->slot_count, r->hdr->slot_data_size / 1024,
	       r->hdr->epoch, r->hdr->version, r->hdr->magic);

	uint8_t *buf = malloc(BUF_SIZE);
	if (!buf) { fprintf(stderr, "OOM\n"); venc_frame_ring_destroy(r); return 1; }
	uint32_t out_len = 0;

	unsigned long total_frames = 0, total_idr = 0, total_bytes = 0;
	uint32_t max_frame = 0, min_frame = 0xFFFFFFFF;
	unsigned long bad_meta = 0, bad_startcode = 0, pts_regress = 0;
	uint32_t first_pts = 0, last_pts = 0;
	int have_first = 0;

	struct timespec ts_start, ts_now, ts_report;
	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	ts_report = ts_start;
	unsigned long interval_frames = 0, interval_idr = 0;

	while (running) {
		int ret = venc_frame_ring_read(r, buf, BUF_SIZE, &out_len);
		if (ret != 0)
			ret = venc_frame_ring_read_wait(r, buf, BUF_SIZE, &out_len, 100);

		if (ret == 0 && out_len >= VENC_FRAME_META_SIZE) {
			VencFrameMeta m;
			memcpy(&m, buf, VENC_FRAME_META_SIZE);
			uint32_t frame_len = out_len - VENC_FRAME_META_SIZE;
			const uint8_t *fd = buf + VENC_FRAME_META_SIZE;
			int is_idr = (m.flags & VENC_FRAME_FLAG_IDR) != 0;

			total_frames++;
			interval_frames++;
			total_bytes += frame_len;
			if (frame_len > max_frame) max_frame = frame_len;
			if (frame_len < min_frame) min_frame = frame_len;
			if (is_idr) { total_idr++; interval_idr++; }

			/* Validate meta: codec H265, reserved 0 */
			if (m.codec != VENC_FRAME_CODEC_H265 || m.reserved != 0)
				bad_meta++;

			/* Validate Annex-B start code (00 00 01 or 00 00 00 01) */
			if (frame_len < 4 ||
			    !((fd[0] == 0 && fd[1] == 0 && fd[2] == 1) ||
			      (fd[0] == 0 && fd[1] == 0 && fd[2] == 0 && fd[3] == 1)))
				bad_startcode++;

			/* pts monotonicity (allow 32-bit wrap tolerance) */
			if (have_first) {
				if (m.pts < last_pts && (last_pts - m.pts) < 0x80000000u)
					pts_regress++;
			} else {
				first_pts = m.pts;
				have_first = 1;
			}
			last_pts = m.pts;
		}

		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		long el_ms = (ts_now.tv_sec - ts_report.tv_sec) * 1000
			+ (ts_now.tv_nsec - ts_report.tv_nsec) / 1000000;
		if (el_ms >= 1000) {
			double s = el_ms / 1000.0;
			printf("  %.1f fps  (%lu IDR)  ring lag=%lu\n",
			       interval_frames / s, interval_idr,
			       (unsigned long)(r->hdr->write_idx - r->hdr->read_idx));
			ts_report = ts_now;
			interval_frames = 0; interval_idr = 0;
		}
		if (duration > 0 && (ts_now.tv_sec - ts_start.tv_sec) >= duration)
			break;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_now);
	double total_s = (ts_now.tv_sec - ts_start.tv_sec)
		+ (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;

	printf("\n=== Frame-SHM Consumer Results ===\n");
	printf("Duration:     %.1f s\n", total_s);
	printf("Frames:       %lu (%.1f fps)\n", total_frames, total_frames / total_s);
	printf("IDR frames:   %lu (every ~%.1f frames)\n", total_idr,
	       total_idr ? (double)total_frames / total_idr : 0.0);
	printf("Data:         %.2f MB (%.2f Mbit/s)\n",
	       total_bytes / (1024.0 * 1024.0),
	       (total_bytes * 8.0) / (total_s * 1000000.0));
	printf("Frame size:   avg %lu  min %u  max %u bytes\n",
	       total_frames ? total_bytes / total_frames : 0,
	       total_frames ? min_frame : 0, max_frame);
	printf("pts:          first=%u last=%u span=%u\n",
	       first_pts, last_pts, last_pts - first_pts);
	printf("Integrity:    bad_meta=%lu bad_startcode=%lu pts_regress=%lu\n",
	       bad_meta, bad_startcode, pts_regress);
	printf("Ring:         w_idx=%lu r_idx=%lu\n",
	       (unsigned long)r->hdr->write_idx, (unsigned long)r->hdr->read_idx);

	int ok = (total_frames > 0 && total_idr > 0 &&
	          bad_meta == 0 && bad_startcode == 0 && pts_regress == 0);
	printf("VERDICT:      %s\n", ok ? "PASS" : "FAIL");

	free(buf);
	venc_frame_ring_destroy(r);
	return ok ? 0 : 2;
}
