#include "venc_rec_writer.h"

#include "test_helpers.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Sink recording: what arrived, in what order, and how slowly it consumed. */
typedef struct {
	pthread_mutex_t lock;
	unsigned        calls;
	unsigned        order_ok;
	unsigned        bytes_ok;
	unsigned        idr_count;
	uint64_t        last_pts;
	unsigned        delay_us;   /* simulate a blocking disk */
	unsigned        first_byte_seq[512];
	unsigned        seq_len;
} SinkState;

static void sink_init(SinkState *s, unsigned delay_us)
{
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	s->order_ok = 1;
	s->bytes_ok = 1;
	s->delay_us = delay_us;
}

static void sink_destroy(SinkState *s)
{
	pthread_mutex_destroy(&s->lock);
}

/* Each AU is filled with a single byte value == its sequence number, so the
 * sink can verify BOTH ordering and that the payload arrived intact. */
static void test_sink(void *ctx, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	SinkState *s = ctx;
	unsigned i;
	uint8_t tag;

	if (s->delay_us)
		usleep(s->delay_us);

	pthread_mutex_lock(&s->lock);
	s->calls++;
	if (pts_90khz < s->last_pts)
		s->order_ok = 0;
	s->last_pts = pts_90khz;
	if (is_idr)
		s->idr_count++;

	tag = len ? au[0] : 0;
	for (i = 0; i < len; ++i) {
		if (au[i] != tag) {
			s->bytes_ok = 0;
			break;
		}
	}
	if (s->seq_len < 512)
		s->first_byte_seq[s->seq_len++] = tag;
	pthread_mutex_unlock(&s->lock);
}

static uint8_t *make_au(uint8_t tag, size_t len)
{
	uint8_t *p = malloc(len);

	if (p)
		memset(p, tag, len);
	return p;
}

/* The contract that matters: push() must not block on a slow sink.  If it
 * did, the drain loop would inherit the disk stall this module exists to
 * remove. */
static int test_writer_push_does_not_block_on_a_slow_sink(void)
{
	VencRecWriter *w = NULL;
	SinkState s;
	struct timespec t0, t1;
	long long elapsed_ms;
	int failures = 0;
	int i;

	sink_init(&s, 20000);  /* 20 ms per AU — a badly stalling disk */
	CHECK("slow sink writer start",
		venc_rec_writer_start(&w, test_sink, &s) == 0 && w != NULL);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < 20; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, i == 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	/* 20 pushes against a sink that needs 400 ms total.  Synchronous would
	 * be >=400 ms; queued should be a small number of ms. */
	CHECK("push returns without waiting on the sink", elapsed_ms < 100);

	venc_rec_writer_stop(w);
	CHECK("stop drains everything queued", s.calls == 20);
	CHECK("slow sink order preserved", s.order_ok == 1);
	CHECK("slow sink payload intact", s.bytes_ok == 1);
	sink_destroy(&s);
	return failures;
}

static int test_writer_preserves_order_and_payload(void)
{
	VencRecWriter *w = NULL;
	SinkState s;
	uint64_t queued = 0, dropped = 0;
	int failures = 0;
	int i, ordered = 1;

	sink_init(&s, 0);
	CHECK("order writer start",
		venc_rec_writer_start(&w, test_sink, &s) == 0);
	for (i = 0; i < 200; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 64), 64,
			(uint64_t)i * 900, (i % 50) == 0);
	venc_rec_writer_stop(w);

	CHECK("order all delivered", s.calls == 200);
	CHECK("order pts monotonic", s.order_ok == 1);
	CHECK("order payload intact", s.bytes_ok == 1);
	CHECK("order idr flag carried", s.idr_count == 4);
	for (i = 0; i < (int)s.seq_len; i++) {
		if (s.first_byte_seq[i] != (unsigned)(i & 0xFF))
			ordered = 0;
	}
	CHECK("order FIFO exactly", ordered == 1);

	/* stats() on a stopped (freed) writer is not valid; check the NULL
	 * contract instead. */
	venc_rec_writer_stats(NULL, &queued, &dropped, NULL, NULL);
	CHECK("stats NULL writer zeroes", queued == 0 && dropped == 0);
	sink_destroy(&s);
	return failures;
}

/* When the disk cannot keep up the queue must bound itself and SAY so — a
 * silent drop would make a damaged recording look like a clean one. */
static int test_writer_drops_and_counts_when_full(void)
{
	VencRecWriter *w = NULL;
	SinkState s;
	uint64_t queued = 0, dropped = 0;
	uint32_t peak = 0;
	int failures = 0;
	int i, accepted = 0, refused = 0;

	sink_init(&s, 50000);  /* 50 ms per AU: the queue will fill */
	CHECK("drop writer start", venc_rec_writer_start(&w, test_sink, &s) == 0);

	/* 16 MB pushed at 256 KB each against a 4 MB cap. */
	for (i = 0; i < 64; i++) {
		if (venc_rec_writer_push(w, make_au((uint8_t)i, 256 * 1024),
				256 * 1024, (uint64_t)i * 900, 0))
			accepted++;
		else
			refused++;
	}
	venc_rec_writer_stats(w, &queued, &dropped, NULL, &peak);

	CHECK("full queue refused some", refused > 0);
	CHECK("full queue accepted some", accepted > 0);
	CHECK("queued counter matches accepted", queued == (uint64_t)accepted);
	CHECK("dropped counter matches refused", dropped == (uint64_t)refused);
	CHECK("queue stayed under its byte cap",
		peak <= VENC_REC_WRITER_MAX_BYTES / (256 * 1024) + 1);

	venc_rec_writer_stop(w);
	CHECK("no leak: sink saw exactly the accepted ones",
		s.calls == (unsigned)accepted);
	CHECK("drop path preserved payload of survivors", s.bytes_ok == 1);
	sink_destroy(&s);
	return failures;
}

/* push() owns the buffer on every path, so a caller can hand it over and
 * forget it.  Under ASan a missed free here is a reported leak. */
static int test_writer_takes_ownership_on_every_path(void)
{
	VencRecWriter *w = NULL;
	SinkState s;
	int failures = 0;

	/* NULL writer still frees — the "failed to start one" path. */
	CHECK("null writer reports drop",
		venc_rec_writer_push(NULL, make_au(1, 32), 32, 0, 0) == 0);

	sink_init(&s, 0);
	CHECK("ownership writer start",
		venc_rec_writer_start(&w, test_sink, &s) == 0);
	CHECK("zero length frees and drops",
		venc_rec_writer_push(w, make_au(2, 32), 0, 0, 0) == 0);
	CHECK("null buffer is a no-op",
		venc_rec_writer_push(w, NULL, 32, 0, 0) == 0);
	venc_rec_writer_stop(w);
	CHECK("no sink call for rejected pushes", s.calls == 0);
	sink_destroy(&s);

	/* stop() on NULL is safe. */
	venc_rec_writer_stop(NULL);
	CHECK("stop NULL safe", 1);
	return failures;
}

static int test_writer_rejects_bad_start(void)
{
	VencRecWriter *w = (VencRecWriter *)0x1;
	int failures = 0;

	CHECK("start needs a sink", venc_rec_writer_start(&w, NULL, NULL) != 0);
	CHECK("start clears out on failure", w == NULL);
	CHECK("start needs an out pointer",
		venc_rec_writer_start(NULL, test_sink, NULL) != 0);
	return failures;
}

int test_venc_rec_writer(void)
{
	int failures = 0;

	failures += test_writer_rejects_bad_start();
	failures += test_writer_takes_ownership_on_every_path();
	failures += test_writer_preserves_order_and_payload();
	failures += test_writer_push_does_not_block_on_a_slow_sink();
	failures += test_writer_drops_and_counts_when_full();
	return failures;
}
