/* Minimal SHM ring consumer for performance measurement.
 * Reads from the venc_wfb ring and reports packet rate + data rate. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#include "venc_ring.h"

static volatile int running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

int main(int argc, char **argv)
{
	const char *name = (argc > 1) ? argv[1] : "venc_wfb";
	int duration = (argc > 2) ? atoi(argv[2]) : 5;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	venc_ring_t *r = venc_ring_attach(name);
	if (!r) {
		fprintf(stderr, "Failed to attach to ring '%s'\n", name);
		return 1;
	}

	printf("Attached to ring '%s': %u slots x %u bytes (epoch=%u, V%u)\n",
	       name, r->hdr->slot_count, r->hdr->slot_data_size,
	       r->hdr->epoch, r->hdr->version);

	uint8_t buf[8192];
	uint16_t out_len;
	unsigned long total_pkts = 0;
	unsigned long total_bytes = 0;

	struct timespec ts_start, ts_now;
	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	struct timespec ts_report = ts_start;
	unsigned long interval_pkts = 0;
	unsigned long interval_bytes = 0;

	while (running) {
		int ret = venc_ring_read(r, buf, sizeof(buf), &out_len);
		if (ret == 0) {
			total_pkts++;
			total_bytes += out_len;
			interval_pkts++;
			interval_bytes += out_len;
		} else {
			/* Ring empty — use futex wait with 100ms timeout */
			ret = venc_ring_read_wait(r, buf, sizeof(buf), &out_len, 100);
			if (ret == 0) {
				total_pkts++;
				total_bytes += out_len;
				interval_pkts++;
				interval_bytes += out_len;
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		long elapsed_ms = (ts_now.tv_sec - ts_report.tv_sec) * 1000
			+ (ts_now.tv_nsec - ts_report.tv_nsec) / 1000000;

		if (elapsed_ms >= 1000) {
			double elapsed_s = elapsed_ms / 1000.0;
			printf("  %lu pkt/s  %.1f Mbit/s\n",
			       (unsigned long)(interval_pkts / elapsed_s),
			       (interval_bytes * 8.0) / (elapsed_s * 1000000.0));
			ts_report = ts_now;
			interval_pkts = 0;
			interval_bytes = 0;
		}

		long total_elapsed = (ts_now.tv_sec - ts_start.tv_sec);
		if (duration > 0 && total_elapsed >= duration)
			break;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_now);
	double total_s = (ts_now.tv_sec - ts_start.tv_sec)
		+ (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;

	printf("\n=== SHM Ring Consumer Results ===\n");
	printf("Duration:   %.1f s\n", total_s);
	printf("Packets:    %lu (%.0f pkt/s)\n", total_pkts, total_pkts / total_s);
	printf("Data:       %.1f MB (%.1f Mbit/s)\n",
	       total_bytes / (1024.0 * 1024.0),
	       (total_bytes * 8.0) / (total_s * 1000000.0));
	printf("Avg pkt:    %lu bytes\n",
	       total_pkts > 0 ? total_bytes / total_pkts : 0);
	printf("Ring w_idx: %lu  r_idx: %lu  (lag: %lu)\n",
	       (unsigned long)r->hdr->write_idx,
	       (unsigned long)r->hdr->read_idx,
	       (unsigned long)(r->hdr->write_idx - r->hdr->read_idx));

	venc_ring_destroy(r);
	return 0;
}
