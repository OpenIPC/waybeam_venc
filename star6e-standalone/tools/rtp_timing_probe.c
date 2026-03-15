/* rtp_timing_probe — host-native RTP + sidecar timing diagnostic tool.
 *
 * The probe is the INITIATOR.  The venc sidecar listener is silent until
 * the probe subscribes.  Traffic stops when the probe exits (subscription
 * TTL expires on the venc side within RTP_SIDECAR_SUB_TTL_US).
 *
 * Protocol roles:
 *   probe → venc   MSG_SUBSCRIBE  (every 2 s)
 *   venc  → probe  MSG_FRAME      (one per frame, sent after last RTP pkt)
 *   probe → venc   MSG_SYNC_REQ   (every 5 s)
 *   venc  → probe  MSG_SYNC_RESP
 *
 * A single UDP socket on the probe is used for both sending and receiving;
 * the venc replies to whatever source address it sees.
 *
 * Per completed frame (RTP marker bit) one TSV line is written to stdout:
 *   frame_no  ssrc  rtp_ts  seq_first  seq_last  gaps
 *   frame_id  frame_ready_us  recv_first_us  recv_last_us  meta_recv_us
 *   sender_interval_us  recv_interval_us  latency_est_us
 *
 * latency_est_us is omitted ("-") until at least one sync round-trip
 * has completed.
 *
 * Build (host, x86-64 / arm64):
 *   cc -std=c99 -Wall -O2 -D_GNU_SOURCE \
 *      -I../include rtp_timing_probe.c -o rtp_timing_probe
 */

#include "rtp_sidecar.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── Tuning constants ────────────────────────────────────────────────── */

#define SUBSCRIBE_INTERVAL_US   (2 * 1000000ULL)  /* resubscribe every 2 s  */
#define SYNC_BURST_INTERVAL_US  (200 * 1000ULL)   /* fast sync during burst */
#define SYNC_COAST_INTERVAL_US  (10 * 1000000ULL) /* drift-tracking sync    */
#define SYNC_BURST_COUNT        8u                 /* samples before coasting*/
#define SYNC_FILTER_SIZE        8u                 /* min-RTT window size    */
#define SIDECAR_CACHE_SIZE      64u                /* must be power-of-2     */
#define MAX_PKT                 2048u

/* ── Monotonic clock (µs) ────────────────────────────────────────────── */

static uint64_t now_us(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ── RTP header ──────────────────────────────────────────────────────── */

typedef struct {
	uint8_t  v_p_x_cc;
	uint8_t  m_pt;
	uint16_t seq;
	uint32_t timestamp;
	uint32_t ssrc;
} RtpHdr;

static int rtp_parse(const uint8_t *buf, size_t len, RtpHdr *out)
{
	if (len < 12 || (buf[0] >> 6) != 2)
		return -1;
	out->v_p_x_cc  = buf[0];
	out->m_pt      = buf[1];
	out->seq       = (uint16_t)((buf[2] << 8) | buf[3]);
	out->timestamp = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
	                 ((uint32_t)buf[6] << 8)  |  (uint32_t)buf[7];
	out->ssrc      = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
	                 ((uint32_t)buf[10] << 8) |  (uint32_t)buf[11];
	return 0;
}

/* ── Sidecar metadata cache ──────────────────────────────────────────── */

typedef struct {
	uint32_t         ssrc;
	uint32_t         rtp_ts;
	uint64_t         frame_id;
	uint64_t         capture_us;        /* venc: encoder PTS (0=unknown)        */
	uint64_t         frame_ready_us;    /* venc: before packetise+send          */
	uint64_t         last_pkt_send_us;  /* venc: after final sendmsg            */
	uint16_t         seq_first;
	uint16_t         seq_count;
	uint64_t         meta_recv_us;      /* probe: received MSG_FRAME            */
	int              valid;
} CacheEntry;

static CacheEntry g_cache[SIDECAR_CACHE_SIZE];

static void cache_store_frame(const RtpSidecarFrame *wire, uint64_t recv_us)
{
	uint32_t ssrc   = ntohl(wire->ssrc);
	uint32_t rtp_ts = ntohl(wire->rtp_timestamp);
	unsigned idx    = (ssrc ^ rtp_ts) & (SIDECAR_CACHE_SIZE - 1u);
	CacheEntry *e   = &g_cache[idx];
	e->ssrc              = ssrc;
	e->rtp_ts            = rtp_ts;
	e->frame_id          = be64toh(wire->frame_id);
	e->capture_us        = be64toh(wire->capture_us);
	e->frame_ready_us    = be64toh(wire->frame_ready_us);
	e->last_pkt_send_us  = be64toh(wire->last_pkt_send_us);
	e->seq_first         = ntohs(wire->seq_first);
	e->seq_count         = ntohs(wire->seq_count);
	e->meta_recv_us      = recv_us;
	e->valid             = 1;
}

static const CacheEntry *cache_lookup(uint32_t ssrc, uint32_t rtp_ts)
{
	unsigned idx  = (ssrc ^ rtp_ts) & (SIDECAR_CACHE_SIZE - 1u);
	const CacheEntry *e = &g_cache[idx];
	return (e->valid && e->ssrc == ssrc && e->rtp_ts == rtp_ts) ? e : NULL;
}

/* ── Per-frame accumulator ───────────────────────────────────────────── */

typedef struct {
	int      active;
	uint32_t ssrc;
	uint32_t rtp_ts;
	uint16_t seq_first;
	uint16_t seq_last;
	uint32_t gaps;
	uint64_t recv_first_us;
	uint64_t recv_last_us;
} FrameAccum;

/* ── Clock-sync state ────────────────────────────────────────────────── */

typedef struct {
	int64_t  rtt_us[SYNC_FILTER_SIZE];
	int64_t  offset_us[SYNC_FILTER_SIZE];
	unsigned count;
	unsigned next;
	int64_t  best_offset_us;
	int      valid;
	uint64_t pending_t1;   /* 0 = no outstanding request */
} SyncState;

static void sync_record(SyncState *ss,
                        uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4)
{
	int64_t rtt    = (int64_t)(t4 - t1) - (int64_t)(t3 - t2);
	int64_t offset = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;

	unsigned i = ss->next & (SYNC_FILTER_SIZE - 1u);
	ss->rtt_us[i]    = rtt;
	ss->offset_us[i] = offset;
	ss->next++;
	if (ss->count < SYNC_FILTER_SIZE)
		ss->count++;

	/* Choose offset from the minimum-RTT sample in the window */
	unsigned n      = ss->count;
	int64_t min_rtt = ss->rtt_us[0];
	int64_t best    = ss->offset_us[0];
	for (unsigned j = 1; j < n; j++) {
		if (ss->rtt_us[j] < min_rtt) {
			min_rtt = ss->rtt_us[j];
			best    = ss->offset_us[j];
		}
	}
	ss->best_offset_us = best;
	ss->valid = 1;
}

/* ── UDP helpers ─────────────────────────────────────────────────────── */

/* Bind a UDP socket to listen_port on INADDR_ANY, non-blocking */
static int udp_bind_nb(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "socket: %s\n", strerror(errno));
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		fprintf(stderr, "fcntl O_NONBLOCK: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	struct sockaddr_in sa = {
		.sin_family      = AF_INET,
		.sin_port        = htons(port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		fprintf(stderr, "bind :%u: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* ── Send subscribe packet ───────────────────────────────────────────── */

static void send_subscribe(int fd, const struct sockaddr_in *venc)
{
	RtpSidecarSubscribe sub;
	sub.magic    = htonl(RTP_SIDECAR_MAGIC);
	sub.version  = RTP_SIDECAR_VERSION;
	sub.msg_type = RTP_SIDECAR_MSG_SUBSCRIBE;
	sub._pad[0]  = 0;
	sub._pad[1]  = 0;
	sendto(fd, &sub, sizeof(sub), MSG_DONTWAIT,
	       (const struct sockaddr *)venc, sizeof(*venc));
}

/* ── Send sync request ───────────────────────────────────────────────── */

static void send_sync_req(int fd, const struct sockaddr_in *venc,
                          SyncState *ss)
{
	uint64_t t1 = now_us();
	RtpSidecarSyncReq req;
	req.magic    = htonl(RTP_SIDECAR_MAGIC);
	req.version  = RTP_SIDECAR_VERSION;
	req.msg_type = RTP_SIDECAR_MSG_SYNC_REQ;
	req._pad[0]  = 0;
	req._pad[1]  = 0;
	req.t1_us    = htobe64(t1);
	ss->pending_t1 = t1;
	sendto(fd, &req, sizeof(req), MSG_DONTWAIT,
	       (const struct sockaddr *)venc, sizeof(*venc));
}

/* ── Output ──────────────────────────────────────────────────────────── */

static void print_header(void)
{
	/*
	 * Columns in timestamp order (venc clock first, then probe clock).
	 * All _us values are CLOCK_MONOTONIC_RAW microseconds.
	 * Venc and probe clocks are independent; use offset_us to compare.
	 *
	 * Sender-side span (requires clock sync for cross-clock comparison):
	 *   capture_us → frame_ready_us          : encode duration
	 *   frame_ready_us → last_pkt_send_us     : packetise + kernel send
	 *
	 * Network + receiver span:
	 *   last_pkt_send_us+offset → recv_last_us: one-way network latency
	 *   recv_first_us → recv_last_us           : within-frame spread
	 *
	 * Full path (requires sync):
	 *   capture_us+offset → recv_last_us       : sensor-to-received latency
	 */
	printf("# frame_no\tssrc\trtp_ts\tseq_first\tseq_last\tseq_count\tgaps\t"
	       "frame_id\t"
	       "capture_us\tframe_ready_us\tlast_pkt_send_us\t"
	       "meta_recv_us\t"
	       "recv_first_us\trecv_last_us\t"
	       "sender_ivl_us\trecv_ivl_us\t"
	       "encode_dur_us\tsend_spread_us\t"
	       "latency_est_us\n");
	fflush(stdout);
}

/*
 * Helper: print a uint64 or "-" if zero (zero = not available).
 * Returns the value printed (or 0).
 */
static uint64_t col_u64(uint64_t v)
{
	if (v)
		printf("%" PRIu64, v);
	else
		printf("-");
	return v;
}

static void emit_frame(unsigned long frame_no,
                       const FrameAccum *fa,
                       const CacheEntry *ce,
                       uint64_t prev_frame_ready_us,
                       uint64_t prev_recv_first_us,
                       const SyncState *ss)
{
	uint64_t sender_ivl    = 0;
	uint64_t recv_ivl      = 0;
	uint64_t encode_dur    = 0;
	uint64_t send_spread   = 0;
	int64_t  latency       = 0;
	int      have_lat      = 0;

	if (ce) {
		if (prev_frame_ready_us)
			sender_ivl = ce->frame_ready_us - prev_frame_ready_us;
		if (ce->capture_us && ce->frame_ready_us > ce->capture_us)
			encode_dur = ce->frame_ready_us - ce->capture_us;
		if (ce->last_pkt_send_us && ce->last_pkt_send_us > ce->frame_ready_us)
			send_spread = ce->last_pkt_send_us - ce->frame_ready_us;
		if (ss->valid) {
			/* offset = ((t2-t1)+(t3-t4))/2 where t2,t3 are venc clock.
			 * To convert venc→probe: probe_time = venc_time - offset */
			latency  = (int64_t)fa->recv_last_us
			           - ((int64_t)ce->frame_ready_us - ss->best_offset_us);
			have_lat = 1;
		}
	}
	if (prev_recv_first_us)
		recv_ivl = fa->recv_first_us - prev_recv_first_us;

	/* Identity columns */
	printf("%lu\t%08x\t%u\t%u\t%u\t",
	       frame_no, fa->ssrc, fa->rtp_ts,
	       fa->seq_first, fa->seq_last);
	if (ce) printf("%u", ce->seq_count); else printf("-");
	printf("\t%u\t", fa->gaps);

	/* frame_id */
	if (ce) printf("%" PRIu64, ce->frame_id); else printf("-");
	printf("\t");

	/* Venc-side timestamps */
	if (ce) { col_u64(ce->capture_us);       } else printf("-"); printf("\t");
	if (ce) { col_u64(ce->frame_ready_us);   } else printf("-"); printf("\t");
	if (ce) { col_u64(ce->last_pkt_send_us); } else printf("-"); printf("\t");

	/* Probe sidecar receive timestamp */
	if (ce) { col_u64(ce->meta_recv_us); } else printf("-"); printf("\t");

	/* Probe RTP receive timestamps */
	printf("%" PRIu64 "\t%" PRIu64 "\t", fa->recv_first_us, fa->recv_last_us);

	/* Intervals */
	if (sender_ivl) printf("%" PRIu64, sender_ivl); else printf("-"); printf("\t");
	if (recv_ivl)   printf("%" PRIu64, recv_ivl);   else printf("-"); printf("\t");

	/* Derived durations */
	if (encode_dur)  printf("%" PRIu64, encode_dur);  else printf("-"); printf("\t");
	if (send_spread) printf("%" PRIu64, send_spread); else printf("-"); printf("\t");

	/* End-to-end latency estimate */
	if (have_lat) printf("%" PRId64 "\n", latency); else printf("-\n");

	fflush(stdout);
}

/* ── Running statistics accumulator ───────────────────────────────────── */

typedef struct {
	unsigned long count;
	uint64_t      sum;
	uint64_t      min;
	uint64_t      max;
	double        m2;     /* Welford online variance */
	double        mean;
} StatAcc;

static void stat_init(StatAcc *s)
{
	memset(s, 0, sizeof(*s));
	s->min = UINT64_MAX;
}

static void stat_add(StatAcc *s, uint64_t v)
{
	s->count++;
	s->sum += v;
	if (v < s->min) s->min = v;
	if (v > s->max) s->max = v;
	double delta = (double)v - s->mean;
	s->mean += delta / (double)s->count;
	double delta2 = (double)v - s->mean;
	s->m2 += delta * delta2;
}

static double stat_stddev(const StatAcc *s)
{
	return s->count > 1 ? sqrt(s->m2 / (double)s->count) : 0.0;
}

/* Percentile from sorted array */
static uint64_t percentile(const uint64_t *sorted, unsigned long n, int pct)
{
	if (n == 0) return 0;
	unsigned long idx = (unsigned long)((double)n * pct / 100.0);
	if (idx >= n) idx = n - 1;
	return sorted[idx];
}

/* qsort comparator */
static int cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

typedef struct {
	int           enabled;
	uint64_t      first_frame_us;
	uint64_t      last_frame_us;
	unsigned long frames;
	unsigned long total_rtp_pkts;
	unsigned long total_gaps;
	unsigned long sidecar_meta_rx;     /* MSG_FRAME received */
	unsigned long sidecar_sub_tx;      /* MSG_SUBSCRIBE sent */
	unsigned long sidecar_sync_tx;     /* MSG_SYNC_REQ sent */
	unsigned long sidecar_sync_rx;     /* MSG_SYNC_RESP received */
	StatAcc       sender_ivl;
	StatAcc       recv_ivl;
	StatAcc       send_spread;
	StatAcc       encode_dur;
	/* For percentile computation, store spread values in a growable array */
	uint64_t     *spread_vals;
	unsigned long spread_cap;
	unsigned long spread_n;
} ProbeStats;

static void stats_init(ProbeStats *ps)
{
	memset(ps, 0, sizeof(*ps));
	stat_init(&ps->sender_ivl);
	stat_init(&ps->recv_ivl);
	stat_init(&ps->send_spread);
	stat_init(&ps->encode_dur);
}

static void stats_add_spread(ProbeStats *ps, uint64_t v)
{
	stat_add(&ps->send_spread, v);
	if (ps->spread_n >= ps->spread_cap) {
		unsigned long new_cap = ps->spread_cap ? ps->spread_cap * 2 : 1024;
		uint64_t *p = realloc(ps->spread_vals, new_cap * sizeof(uint64_t));
		if (!p) return;
		ps->spread_vals = p;
		ps->spread_cap = new_cap;
	}
	ps->spread_vals[ps->spread_n++] = v;
}

static void stats_print(const ProbeStats *ps, const SyncState *ss)
{
	uint64_t elapsed_us = (ps->last_frame_us > ps->first_frame_us)
		? ps->last_frame_us - ps->first_frame_us : 0;
	double elapsed_s = (double)elapsed_us / 1e6;

	fprintf(stderr, "\n=== Timing Probe Summary ===\n\n");
	fprintf(stderr, "Duration:             %.1f s\n", elapsed_s);
	fprintf(stderr, "Frames:               %lu (%.1f fps)\n",
	        ps->frames, elapsed_s > 0 ? (double)ps->frames / elapsed_s : 0);
	fprintf(stderr, "RTP packets:          %lu (%.1f avg/frame)\n",
	        ps->total_rtp_pkts,
	        ps->frames > 0 ? (double)ps->total_rtp_pkts / (double)ps->frames : 0);
	fprintf(stderr, "RTP gaps:             %lu\n", ps->total_gaps);

	fprintf(stderr, "\n--- Sidecar overhead ---\n");
	unsigned long sc_rx = ps->sidecar_meta_rx + ps->sidecar_sync_rx;
	unsigned long sc_tx = ps->sidecar_sub_tx + ps->sidecar_sync_tx;
	uint64_t rx_bytes = ps->sidecar_meta_rx * (uint64_t)sizeof(RtpSidecarFrame)
	                  + ps->sidecar_sync_rx * (uint64_t)sizeof(RtpSidecarSyncResp);
	uint64_t tx_bytes = ps->sidecar_sub_tx * (uint64_t)sizeof(RtpSidecarSubscribe)
	                  + ps->sidecar_sync_tx * (uint64_t)sizeof(RtpSidecarSyncReq);
	fprintf(stderr, "  venc->probe:        %lu pkts (%lu frame + %lu sync)\n",
	        sc_rx, ps->sidecar_meta_rx, ps->sidecar_sync_rx);
	fprintf(stderr, "  probe->venc:        %lu pkts (%lu subscribe + %lu sync)\n",
	        sc_tx, ps->sidecar_sub_tx, ps->sidecar_sync_tx);
	if (elapsed_s > 0) {
		fprintf(stderr, "  Bandwidth:          %.1f KB/s rx, %.1f KB/s tx (%.1f kbps total)\n",
		        (double)rx_bytes / elapsed_s / 1024.0,
		        (double)tx_bytes / elapsed_s / 1024.0,
		        (double)(rx_bytes + tx_bytes) * 8.0 / elapsed_s / 1000.0);
		fprintf(stderr, "  Packet rate:        %.0f pps rx, %.1f pps tx\n",
		        (double)sc_rx / elapsed_s, (double)sc_tx / elapsed_s);
	}

	if (ps->sender_ivl.count > 0) {
		fprintf(stderr, "\n--- Frame intervals (sender clock) ---\n");
		fprintf(stderr, "  Mean:    %.0f us  (%.1f fps)\n",
		        ps->sender_ivl.mean,
		        ps->sender_ivl.mean > 0 ? 1e6 / ps->sender_ivl.mean : 0);
		fprintf(stderr, "  Min:     %" PRIu64 " us\n", ps->sender_ivl.min);
		fprintf(stderr, "  Max:     %" PRIu64 " us\n", ps->sender_ivl.max);
		fprintf(stderr, "  Stddev:  %.0f us\n", stat_stddev(&ps->sender_ivl));
	}

	if (ps->recv_ivl.count > 0) {
		fprintf(stderr, "\n--- Frame intervals (probe clock) ---\n");
		fprintf(stderr, "  Mean:    %.0f us  (%.1f fps)\n",
		        ps->recv_ivl.mean,
		        ps->recv_ivl.mean > 0 ? 1e6 / ps->recv_ivl.mean : 0);
		fprintf(stderr, "  Stddev:  %.0f us\n", stat_stddev(&ps->recv_ivl));
	}

	if (ps->send_spread.count > 0) {
		/* Sort for percentiles */
		uint64_t *sorted = malloc(ps->spread_n * sizeof(uint64_t));
		if (sorted) {
			memcpy(sorted, ps->spread_vals, ps->spread_n * sizeof(uint64_t));
			qsort(sorted, ps->spread_n, sizeof(uint64_t), cmp_u64);

			fprintf(stderr, "\n--- Send spread (frame_ready -> last_pkt_send) ---\n");
			fprintf(stderr, "  Mean:    %.0f us\n", ps->send_spread.mean);
			fprintf(stderr, "  Min:     %" PRIu64 " us\n", ps->send_spread.min);
			fprintf(stderr, "  P50:     %" PRIu64 " us\n",
			        percentile(sorted, ps->spread_n, 50));
			fprintf(stderr, "  P95:     %" PRIu64 " us\n",
			        percentile(sorted, ps->spread_n, 95));
			fprintf(stderr, "  P99:     %" PRIu64 " us\n",
			        percentile(sorted, ps->spread_n, 99));
			fprintf(stderr, "  Max:     %" PRIu64 " us\n", ps->send_spread.max);
			free(sorted);
		}
	}

	if (ps->encode_dur.count > 0) {
		fprintf(stderr, "\n--- Encode duration (capture -> frame_ready) ---\n");
		fprintf(stderr, "  Mean:    %.0f us\n", ps->encode_dur.mean);
		fprintf(stderr, "  Min:     %" PRIu64 " us\n", ps->encode_dur.min);
		fprintf(stderr, "  Max:     %" PRIu64 " us\n", ps->encode_dur.max);
	}

	if (ss->valid) {
		fprintf(stderr, "\n--- Clock sync ---\n");
		fprintf(stderr, "  Samples:  %u\n", ss->count);
		fprintf(stderr, "  Offset:   %+" PRId64 " us\n", ss->best_offset_us);
		int64_t min_rtt = ss->rtt_us[0];
		for (unsigned i = 1; i < ss->count; i++)
			if (ss->rtt_us[i] < min_rtt) min_rtt = ss->rtt_us[i];
		fprintf(stderr, "  Best RTT: %" PRId64 " us\n", min_rtt);
	}

	fprintf(stderr, "\n");
}

/* ── Signal ──────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	uint16_t    rtp_port    = 5600;
	uint16_t    sidecar_port = 6666;
	const char *venc_ip     = NULL;

	int stats_enabled = 0;

	for (int i = 1; i < argc; i++) {
		if ((!strcmp(argv[i], "--rtp-port") ||
		     !strcmp(argv[i], "--sidecar-port")) && i + 1 < argc) {
			char *end;
			errno = 0;
			unsigned long v = strtoul(argv[i + 1], &end, 10);
			if (errno || *end || v == 0 || v > 65535) {
				fprintf(stderr, "invalid port '%s'\n", argv[i + 1]);
				return 1;
			}
			if (!strcmp(argv[i], "--rtp-port"))
				rtp_port = (uint16_t)v;
			else
				sidecar_port = (uint16_t)v;
			i++;
		} else if (!strcmp(argv[i], "--venc-ip") && i + 1 < argc)
			venc_ip = argv[++i];
		else if (!strcmp(argv[i], "--stats"))
			stats_enabled = 1;
		else if (!strcmp(argv[i], "--help")) {
			printf("Usage: rtp_timing_probe [options]\n"
			       "  --rtp-port     <port>  RTP listen port (default 5600)\n"
			       "  --sidecar-port <port>  Sidecar port on venc (default 6666)\n"
			       "  --venc-ip      <ip>    venc IP address (required)\n"
			       "  --stats                Print summary statistics on exit\n"
			       "\n"
			       "The probe initiates all traffic.  The venc sidecar is silent\n"
			       "until MSG_SUBSCRIBE arrives; it stops within %u s after the\n"
			       "probe exits.\n",
			       (unsigned)(RTP_SIDECAR_SUB_TTL_US / 1000000ULL));
			return 0;
		}
	}

	if (!venc_ip) {
		fprintf(stderr, "error: --venc-ip <ip> is required\n");
		return 1;
	}

	/* venc destination address (for subscribe + sync requests) */
	struct sockaddr_in venc_addr;
	memset(&venc_addr, 0, sizeof(venc_addr));
	venc_addr.sin_family = AF_INET;
	venc_addr.sin_port   = htons(sidecar_port);
	if (inet_pton(AF_INET, venc_ip, &venc_addr.sin_addr) != 1) {
		fprintf(stderr, "invalid --venc-ip '%s'\n", venc_ip);
		return 1;
	}

	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);

	/*
	 * Two sockets:
	 *   rtp_fd     — receives the RTP video stream (inbound only)
	 *   sidecar_fd — sends subscribe/sync to venc AND receives frame
	 *                metadata + sync responses back on the same port
	 */
	int rtp_fd     = udp_bind_nb(rtp_port);
	int sidecar_fd = udp_bind_nb(0);   /* OS assigns a local port */
	if (rtp_fd < 0 || sidecar_fd < 0)
		return 1;

	/* Log the OS-assigned local sidecar port (needed for firewall rules) */
	uint16_t local_sidecar_port = 0;
	{
		struct sockaddr_in sa;
		socklen_t sa_len = sizeof(sa);
		if (getsockname(sidecar_fd, (struct sockaddr *)&sa, &sa_len) == 0)
			local_sidecar_port = ntohs(sa.sin_port);
	}

	fprintf(stderr,
	        "[probe] RTP :%u  sidecar local :%u → %s:%u  sub-TTL %u s\n",
	        rtp_port, local_sidecar_port, venc_ip, sidecar_port,
	        (unsigned)(RTP_SIDECAR_SUB_TTL_US / 1000000ULL));

	print_header();

	uint8_t       buf[MAX_PKT];
	FrameAccum    fa             = {0};
	SyncState     ss             = {0};
	ProbeStats    ps;
	unsigned long frame_no       = 0;
	uint64_t      prev_frame_ready = 0;
	uint64_t      prev_recv_first  = 0;
	uint64_t      last_subscribe   = 0;
	uint64_t      last_sync        = 0;

	stats_init(&ps);
	ps.enabled = stats_enabled;

	while (g_running) {
		uint64_t now = now_us();

		/* ── Periodic subscribe (initial + keepalive) ───────────── */
		if (now - last_subscribe >= SUBSCRIBE_INTERVAL_US) {
			send_subscribe(sidecar_fd, &venc_addr);
			ps.sidecar_sub_tx++;
			last_subscribe = now;
		}

		/* ── Periodic clock-sync request ────────────────────────── */
		{
			uint64_t sync_ivl = ss.count < SYNC_BURST_COUNT
				? SYNC_BURST_INTERVAL_US
				: SYNC_COAST_INTERVAL_US;
			if (now - last_sync >= sync_ivl) {
				send_sync_req(sidecar_fd, &venc_addr, &ss);
				ps.sidecar_sync_tx++;
				last_sync = now;
			}
		}

		/* ── Wait for incoming data (1 s max so timers fire) ────── */
		fd_set rset;
		FD_ZERO(&rset);
		FD_SET(rtp_fd, &rset);
		FD_SET(sidecar_fd, &rset);
		int maxfd = (rtp_fd > sidecar_fd ? rtp_fd : sidecar_fd) + 1;

		struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
		int r = select(maxfd, &rset, NULL, NULL, &tv);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			continue;

		/* ── Sidecar packet ──────────────────────────────────────── */
		if (FD_ISSET(sidecar_fd, &rset)) {
			ssize_t n = recv(sidecar_fd, buf, sizeof(buf), MSG_DONTWAIT);
			uint64_t recv_ts = now_us();

			if (n >= 6) {
				const uint32_t *magic_p = (const uint32_t *)buf;
				if (ntohl(*magic_p) == RTP_SIDECAR_MAGIC &&
				    buf[4] == RTP_SIDECAR_VERSION) {

					uint8_t msg_type = buf[5];

					if (msg_type == RTP_SIDECAR_MSG_FRAME &&
					    (size_t)n >= sizeof(RtpSidecarFrame)) {
						cache_store_frame((const RtpSidecarFrame *)buf, recv_ts);
						ps.sidecar_meta_rx++;

					} else if (msg_type == RTP_SIDECAR_MSG_SYNC_RESP &&
					           (size_t)n >= sizeof(RtpSidecarSyncResp) &&
					           ss.pending_t1) {
						const RtpSidecarSyncResp *resp =
						    (const RtpSidecarSyncResp *)buf;
						uint64_t t1 = be64toh(resp->t1_us);
						uint64_t t2 = be64toh(resp->t2_us);
						uint64_t t3 = be64toh(resp->t3_us);
						if (t1 == ss.pending_t1) {
							sync_record(&ss, t1, t2, t3, recv_ts);
							ps.sidecar_sync_rx++;
							ss.pending_t1 = 0;
							fprintf(stderr,
							    "[sync] offset=%+" PRId64
							    " µs  rtt=%" PRId64 " µs\n",
							    ss.best_offset_us,
							    ss.rtt_us[(ss.next - 1) &
							        (SYNC_FILTER_SIZE - 1u)]);
						}
					}
				}
			}
		}

		/* ── RTP packet ─────────────────────────────────────────── */
		if (FD_ISSET(rtp_fd, &rset)) {
			ssize_t n = recv(rtp_fd, buf, sizeof(buf), MSG_DONTWAIT);
			uint64_t recv_ts = now_us();
			if (n < 12)
				continue;

			RtpHdr hdr;
			if (rtp_parse(buf, (size_t)n, &hdr) != 0)
				continue;

			int marker = (hdr.m_pt >> 7) & 1;

			if (!fa.active) {
				fa.active        = 1;
				fa.ssrc          = hdr.ssrc;
				fa.rtp_ts        = hdr.timestamp;
				fa.seq_first     = hdr.seq;
				fa.seq_last      = hdr.seq;
				fa.gaps          = 0;
				fa.recv_first_us = recv_ts;
				fa.recv_last_us  = recv_ts;
			} else {
				uint16_t expected = (uint16_t)(fa.seq_last + 1u);
				if (hdr.seq != expected)
					fa.gaps += (uint16_t)(hdr.seq - expected);
				fa.seq_last     = hdr.seq;
				fa.recv_last_us = recv_ts;
			}

			if (marker) {
				const CacheEntry *ce = cache_lookup(fa.ssrc, fa.rtp_ts);
				emit_frame(frame_no++, &fa, ce,
				           prev_frame_ready, prev_recv_first, &ss);

				/* Accumulate stats (skip realloc/work when --stats not given) */
				if (ps.enabled) {
					if (ps.first_frame_us == 0)
						ps.first_frame_us = fa.recv_first_us;
					ps.last_frame_us = fa.recv_last_us;
					ps.frames++;
					ps.total_rtp_pkts += (uint16_t)(fa.seq_last - fa.seq_first) + 1u;
					ps.total_gaps += fa.gaps;
					if (ce) {
						if (prev_frame_ready && ce->frame_ready_us > prev_frame_ready)
							stat_add(&ps.sender_ivl, ce->frame_ready_us - prev_frame_ready);
						if (ce->capture_us && ce->frame_ready_us > ce->capture_us)
							stat_add(&ps.encode_dur, ce->frame_ready_us - ce->capture_us);
						if (ce->last_pkt_send_us && ce->last_pkt_send_us > ce->frame_ready_us)
							stats_add_spread(&ps, ce->last_pkt_send_us - ce->frame_ready_us);
					}
					if (prev_recv_first && fa.recv_first_us > prev_recv_first)
						stat_add(&ps.recv_ivl, fa.recv_first_us - prev_recv_first);
				}

				if (ce)
					prev_frame_ready = ce->frame_ready_us;
				prev_recv_first = fa.recv_first_us;
				fa.active = 0;
			}
		}
	}

	if (ps.enabled)
		stats_print(&ps, &ss);

	fprintf(stderr, "[probe] exiting — venc subscription expires in %u s\n",
	        (unsigned)(RTP_SIDECAR_SUB_TTL_US / 1000000ULL));

	free(ps.spread_vals);
	close(rtp_fd);
	close(sidecar_fd);
	return 0;
}
