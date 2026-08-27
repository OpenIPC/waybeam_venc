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

/* The barrier contract.  drain() exists so a caller can release the resource
 * the sink writes into — close the segment file, free the recorder state — the
 * instant it returns.  If the sink can still run afterwards, the previous
 * recording's tail lands in the NEXT file, over its PAT/PMT and opening IRAP.
 *
 * This models exactly that: the sink touches a resource the main thread
 * releases the moment drain() returns.  Against an unlocked-mid-write pop it
 * fails, because head == NULL is true for the whole duration of the write. */
typedef struct {
	unsigned delay_us;
	int      released;            /* main thread releases after drain() */
	int      ran_after_release;   /* the defect, if it ever gets set */
	unsigned calls;
} BarrierSink;

static void barrier_sink(void *ctx, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	BarrierSink *s = ctx;

	(void)au; (void)len; (void)pts_90khz; (void)is_idr;

	/* A blocking write(2) on a stalling card.  The sink reads the resource
	 * on the far side of it, which is where the real recorder touches the
	 * fd it was handed. */
	usleep(s->delay_us);
	if (__atomic_load_n(&s->released, __ATOMIC_ACQUIRE))
		s->ran_after_release = 1;
	s->calls++;
}

static int test_writer_drain_is_a_real_barrier(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	int failures = 0;

	memset(&s, 0, sizeof(s));
	s.delay_us = 150000;   /* 150 ms — one slow SD-card write */

	CHECK("barrier writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0 && w != NULL);

	(void)venc_rec_writer_push(w, make_au(1, 1024), 1024, 900, 1);

	/* Let the writer thread actually pick the entry up, so the queue is
	 * empty but the sink is mid-write — the state drain() must not mistake
	 * for "done". */
	usleep(20000);

	venc_rec_writer_drain(w, 2000, NULL);

	/* Everything after this line stands for close(fd) / recorder teardown. */
	__atomic_store_n(&s.released, 1, __ATOMIC_RELEASE);

	/* Join before asserting: the question is not whether the sink has run
	 * YET, it is whether it ran BEFORE the release.  Reading the flags
	 * without the join would only re-measure drain()'s own timing. */
	venc_rec_writer_stop(w);

	CHECK("drain did not lose the frame", s.calls == 1);
	CHECK("drain waited for the in-flight sink", s.ran_after_release == 0);
	return failures;
}

/* The other half of the contract: the barrier is BOUNDED.  A stalled card must
 * not park the encode loop for the length of the queue. */
static int test_writer_drain_is_bounded_and_counts_the_abandoned(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	struct timespec t0, t1;
	long long elapsed_ms;
	uint64_t dropped = 0;
	int failures = 0;
	int i;

	memset(&s, 0, sizeof(s));
	s.delay_us = 120000;   /* 120 ms each: 8 cannot fit a 150 ms grace */

	CHECK("bounded drain writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0);

	for (i = 0; i < 8; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_drain(w, 150, &dropped);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	/* Grace + at most one in-flight sink.  Never the whole backlog (960 ms). */
	CHECK("drain is bounded by grace plus one write", elapsed_ms < 400);
	CHECK("drain counted what it abandoned", dropped > 0);

	venc_rec_writer_stop(w);
	CHECK("abandoned frames never reached the sink", s.calls < 8);
	return failures;
}

/* Counters are per-RECORDING, not per-process.  A long-lived writer spans
 * many recordings; without a reset, a clean one reports the previous one's
 * drops and every timeout poisons the count for good. */
static int test_writer_reset_counters_makes_them_per_recording(void)
{
	VencRecWriter *w = NULL;
	SinkState s;
	uint64_t queued = 0, dropped = 0;
	uint32_t peak = 0;
	int failures = 0;
	int i;

	sink_init(&s, 50000);   /* slow enough to fill the 4 MB cap */
	CHECK("reset writer start", venc_rec_writer_start(&w, test_sink, &s) == 0);

	/* Recording 1: overrun the queue so it sheds frames. */
	for (i = 0; i < 40; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 256 * 1024),
			256 * 1024, (uint64_t)i * 900, 0);
	venc_rec_writer_stats(w, &queued, &dropped, NULL, &peak);
	CHECK("first recording shed frames", dropped > 0);
	CHECK("first recording queued frames", queued > 0);
	CHECK("first recording saw depth", peak > 0);

	/* Recording 2 starts: the slate is clean. */
	venc_rec_writer_reset_counters(w);
	venc_rec_writer_stats(w, &queued, &dropped, NULL, &peak);
	CHECK("second recording inherits no drops", dropped == 0);
	CHECK("second recording inherits no queued count", queued == 0);

	/* peak is reset to the LIVE depth, not to zero: a queue that is still
	 * carrying entries has genuinely reached that depth in this recording. */
	{
		uint32_t depth = 0;

		venc_rec_writer_stats(w, NULL, NULL, &depth, &peak);
		CHECK("peak restarts from the live depth", peak == depth);
	}

	venc_rec_writer_reset_counters(NULL);
	CHECK("reset NULL safe", 1);

	venc_rec_writer_stop(w);
	sink_destroy(&s);
	return failures;
}

/* The pattern both SigmaStar backends use for the case drain() deliberately
 * does NOT cover: the grace expires with an access unit still in the sink's
 * hands.  drain() bounds the wait; a state lock — taken by the sink around
 * its write and by the closer around the close — makes the close safe, at a
 * cost of at most one write rather than the whole queue.
 *
 * Modelled here so the pattern itself is pinned: drop the lock from the sink
 * and this reports a write into a closed recorder (verified by mutation).
 *
 * The lock must be taken AFTER the drain, never around it: the drain waits on
 * the writer thread, and the writer thread needs the same lock to finish. */
typedef struct {
	pthread_mutex_t state_lock;   /* stands for rec_state_lock */
	unsigned        delay_us;
	int             fd_open;      /* stands for the recorder's descriptor */
	int             wrote_while_closed;
	unsigned        writes;
} LockedRecorder;

static void locked_recorder_sink(void *ctx, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	LockedRecorder *r = ctx;

	(void)au; (void)len; (void)pts_90khz; (void)is_idr;

	pthread_mutex_lock(&r->state_lock);
	usleep(r->delay_us);
	if (!r->fd_open)
		r->wrote_while_closed = 1;
	r->writes++;
	pthread_mutex_unlock(&r->state_lock);
}

static int test_writer_state_lock_covers_the_drain_timeout(void)
{
	VencRecWriter *w = NULL;
	LockedRecorder r;
	struct timespec t0, t1;
	long long elapsed_ms;
	uint64_t dropped = 0;
	int failures = 0;
	int i;

	memset(&r, 0, sizeof(r));
	pthread_mutex_init(&r.state_lock, NULL);
	r.delay_us = 60000;   /* 60 ms per write */
	r.fd_open = 1;

	CHECK("state lock writer start",
		venc_rec_writer_start(&w, locked_recorder_sink, &r) == 0);

	/* Far more than a 100 ms grace can absorb, so the drain is guaranteed
	 * to time out with the sink still running. */
	for (i = 0; i < 20; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_drain(w, 100, &dropped);
	CHECK("the drain did time out", dropped > 0);

	/* Exactly what the backends do: close under the state lock. */
	pthread_mutex_lock(&r.state_lock);
	r.fd_open = 0;
	pthread_mutex_unlock(&r.state_lock);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	CHECK("no write landed after the close", r.wrote_while_closed == 0);
	/* Grace + one in-flight write, not the 1.2 s the queue would have
	 * taken.  This is the bound the encode loop is paying for. */
	CHECK("the close waited for one write, not the queue", elapsed_ms < 400);

	venc_rec_writer_stop(w);
	CHECK("still no write after the close", r.wrote_while_closed == 0);
	pthread_mutex_destroy(&r.state_lock);
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
	failures += test_writer_drain_is_a_real_barrier();
	failures += test_writer_drain_is_bounded_and_counts_the_abandoned();
	failures += test_writer_reset_counters_makes_them_per_recording();
	failures += test_writer_state_lock_covers_the_drain_timeout();
	return failures;
}
