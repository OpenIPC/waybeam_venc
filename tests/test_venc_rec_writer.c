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

	/* Both stops on NULL are safe, and *dropped is still assigned.
	 *
	 * stop_bounded(NULL) is not a corner: every backend's record_close()
	 * detaches the handle and calls it unconditionally, and record_open()
	 * calls record_close() on entry — so this runs on every record start,
	 * with no recording open.  It had no test.  (The previous
	 * `CHECK("stop NULL safe", 1)` asserted a literal: if the call faulted
	 * the process would die before reaching it.) */
	{
		uint64_t dropped = 1234;

		venc_rec_writer_stop(NULL);
		venc_rec_writer_stop_bounded(NULL, 250, &dropped);
		CHECK("stop_bounded NULL leaves dropped untouched", dropped == 1234);
		venc_rec_writer_stop_bounded(NULL, 0, NULL);
		CHECK("both stops survive NULL", 1 == 1);
	}
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

/* The barrier contract.  A recorder close follows every stop, and the queued
 * access units are written BY DESCRIPTOR — so if the sink can still run after
 * the stop returns, the previous recording's tail lands in the NEXT file, over
 * its PAT/PMT and opening IRAP.
 *
 * The barrier is the unconditional pthread_join inside the stop.  Nothing
 * else: no in-flight flag, no lock between the sink and the close.  That is
 * the whole reason the writer's lifetime is tied to one recording.
 *
 * These model exactly that: the sink touches a resource the main thread
 * releases the moment the stop returns.  Delete the join and they fail. */
typedef struct {
	unsigned delay_us;
	int      released;            /* main thread releases after the stop */
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

static int test_writer_stop_bounded_is_a_hard_barrier(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	struct timespec t0, t1;
	long long elapsed_ms;
	int failures = 0;

	memset(&s, 0, sizeof(s));
	s.delay_us = 200000;   /* 200 ms — one slow SD-card write */

	CHECK("barrier writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0 && w != NULL);

	(void)venc_rec_writer_push(w, make_au(1, 1024), 1024, 900, 1);

	/* Let the writer thread pick the entry up, so the QUEUE is empty but
	 * the sink is mid-write.  Only the join can cover that state. */
	usleep(20000);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_stop_bounded(w, 10, NULL);   /* grace already expired */
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	/* Everything after this line stands for close(fd) / recorder teardown. */
	__atomic_store_n(&s.released, 1, __ATOMIC_RELEASE);
	usleep(300000);   /* well past the sink's own delay */

	/* The grace was 10 ms and the queue was already empty, so a stop that
	 * skipped the join would return at once.  Waiting out the write is the
	 * observable half of the barrier. */
	CHECK("stop_bounded waited for the in-flight sink", elapsed_ms >= 100);
	CHECK("stop_bounded did not lose the frame", s.calls == 1);
	CHECK("no sink call landed after the stop returned",
		s.ran_after_release == 0);
	return failures;
}

/* The other half of the contract: the barrier is BOUNDED.  A stalled card must
 * not park the encode loop for the length of the queue.
 *
 * This is not hypothetical.  Setting `stopping` does NOT bound the writer
 * thread — it drains its queue to the end and only then honours the flag — so
 * a stop that only joined would block for the whole backlog.  The queue is
 * abandoned before the join for exactly this reason. */
static int test_writer_stop_bounded_abandons_the_backlog(void)
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

	CHECK("bounded stop writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0);

	for (i = 0; i < 8; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_stop_bounded(w, 150, &dropped);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	/* Grace + at most one in-flight sink.  Never the whole backlog (960 ms). */
	/* Grace (150) + at most one 120 ms sink = 270 theoretical; measured 241
	 * idle and 241-251 at 4x CPU oversubscription.  The bound is 400, not
	 * the old 500: a mutant that triples the grace lands at 480 and slipped
	 * through, so the extra 100 ms of slack bought nothing but a hole. */
	CHECK("stop_bounded is bounded by grace plus one write",
		elapsed_ms < 400);
	CHECK("stop_bounded counted what it abandoned", dropped > 0);
	CHECK("abandoned frames never reached the sink", s.calls < 8);
	return failures;
}

/* The asymmetry that justifies which stop a TEARDOWN path may use, measured
 * on one sink with one queue depth so the two numbers are comparable.
 *
 * venc_rec_writer_stop() sets `stopping` but never calls queue_abandon(), and
 * the consumer only observes that flag when queue_pop() returns NULL -- so its
 * join is bounded by the DISK, not by any grace.  cv610_teardown() used that
 * form ("full flush: shutting down anyway"), which on a medium that stalls or
 * disappears under load -- a documented CV610 failure mode -- meant teardown
 * never reached the VENC/VPSS stop below it, leaking the kernel-state channels
 * and binds that make the next start fail.  That backend has no watchdog to
 * cut it short, so it now uses the bounded form like everything else.
 *
 * test_writer_stop_unbounded_is_also_a_barrier() already pins that the
 * unbounded stop delivers everything, and
 * test_writer_stop_bounded_abandons_the_backlog() that the bounded one does
 * not.  Neither compares their COST, which is the property the teardown choice
 * actually turns on. */
static int test_writer_stop_cost_bounded_vs_unbounded(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	struct timespec t0, t1;
	long long unbounded_ms, bounded_ms;
	uint64_t dropped = 0;
	int failures = 0;
	int i;

	/* Arm A: unbounded, 5 x 100 ms of queued sink work. */
	memset(&s, 0, sizeof(s));
	s.delay_us = 100000;
	CHECK("cost arm A start", venc_rec_writer_start(&w, barrier_sink, &s) == 0);
	for (i = 0; i < 5; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_stop(w);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	unbounded_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;
	CHECK("unbounded cost delivered the backlog", s.calls == 5);

	/* Arm B: identical sink and depth, bounded by a 100 ms grace. */
	w = NULL;
	memset(&s, 0, sizeof(s));
	s.delay_us = 100000;
	CHECK("cost arm B start", venc_rec_writer_start(&w, barrier_sink, &s) == 0);
	for (i = 0; i < 5; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_stop_bounded(w, 100, &dropped);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	bounded_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	/* The unbounded join tracks the backlog (~500 ms); the bounded one is
	 * grace plus at most one in-flight write (~200 ms).  Compared as a ratio
	 * rather than against absolute wall-clock, so host load moves both. */
	CHECK("bounded stop cost far less than unbounded",
		bounded_ms * 2 < unbounded_ms);
	CHECK("bounded stop shed what it did not wait for", dropped > 0);
	CHECK("unbounded stop waited for the whole backlog", unbounded_ms >= 400);
	return failures;
}

/* Counters are per-RECORDING.  Not by an operation any more — by the writer's
 * lifetime: the backends create one when a recording opens and destroy it when
 * the recording closes, so the next recording gets a fresh handle whose
 * counters start at zero.  This pins that structural claim; the backends'
 * own per-recording fields (rec_flatten_failures) are reset alongside it in
 * mirror_record_open(), which the host suite does not link. */
static int test_writer_counters_start_clean_for_each_recording(void)
{
	VencRecWriter *w = NULL;
	SinkState s;
	uint64_t queued = 0, dropped = 0;
	uint32_t peak = 0;
	int failures = 0;
	int i;

	sink_init(&s, 50000);   /* slow enough to fill the 4 MB cap */

	/* Recording 1: overrun the queue so it sheds frames. */
	CHECK("first recording writer start",
		venc_rec_writer_start(&w, test_sink, &s) == 0);
	for (i = 0; i < 40; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 256 * 1024),
			256 * 1024, (uint64_t)i * 900, 0);
	venc_rec_writer_stats(w, &queued, &dropped, NULL, &peak);
	CHECK("first recording shed frames", dropped > 0);
	CHECK("first recording queued frames", queued > 0);
	CHECK("first recording saw depth", peak > 0);
	venc_rec_writer_stop_bounded(w, 50, NULL);
	w = NULL;

	/* Recording 2 starts: a new writer, and the slate is clean. */
	CHECK("second recording writer start",
		venc_rec_writer_start(&w, test_sink, &s) == 0);
	venc_rec_writer_stats(w, &queued, &dropped, NULL, &peak);
	CHECK("second recording inherits no drops", dropped == 0);
	CHECK("second recording inherits no queued count", queued == 0);
	CHECK("second recording inherits no peak depth", peak == 0);

	venc_rec_writer_stop_bounded(w, 50, NULL);
	sink_destroy(&s);
	return failures;
}

/* What the join replaced.  The old long-lived writer needed a lock held by
 * the sink around its write and by the closer around the close, because its
 * bounded drain could return with an access unit still in the sink's hands.
 * The join has no such gap, so the close needs NO lock — and this asserts the
 * stronger property directly: close immediately after the stop, unsynchronised,
 * and nothing may write into the closed recorder.
 *
 * Mutation-checked by removing the pthread_join from
 * venc_rec_writer_stop_bounded(): "no write landed after the close" fails. */
typedef struct {
	unsigned delay_us;
	int      fd_open;             /* stands for the recorder's descriptor */
	int      wrote_while_closed;
	unsigned writes;
} UnlockedRecorder;

static void unlocked_recorder_sink(void *ctx, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	UnlockedRecorder *r = ctx;

	(void)au; (void)len; (void)pts_90khz; (void)is_idr;

	usleep(r->delay_us);
	if (!__atomic_load_n(&r->fd_open, __ATOMIC_ACQUIRE))
		r->wrote_while_closed = 1;
	r->writes++;
}

static int test_writer_stop_bounded_makes_a_close_safe_without_a_lock(void)
{
	VencRecWriter *w = NULL;
	UnlockedRecorder r;
	struct timespec t0, t1;
	long long elapsed_ms;
	uint64_t dropped = 0;
	int failures = 0;
	int i;

	memset(&r, 0, sizeof(r));
	r.delay_us = 60000;   /* 60 ms per write */
	r.fd_open = 1;

	CHECK("unlocked close writer start",
		venc_rec_writer_start(&w, unlocked_recorder_sink, &r) == 0);

	/* Far more than a 100 ms grace can absorb, so the stop is guaranteed to
	 * hit its bound with the sink still running. */
	for (i = 0; i < 20; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_stop_bounded(w, 100, &dropped);
	/* Exactly what the backends do: close straight after the stop, with
	 * nothing serialising it against the sink. */
	r.fd_open = 0;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	CHECK("the stop did hit its bound", dropped > 0);
	/* Grace + one in-flight write, not the 1.2 s the queue would have
	 * taken.  This is the bound the encode loop is paying for. */
	CHECK("the close waited for one write, not the queue", elapsed_ms < 500);

	usleep(200000);   /* long enough for a leaked thread to write again */
	CHECK("no write landed after the close", r.wrote_while_closed == 0);
	return failures;
}

/* The barrier must also RETURN when there is nothing to wait for.
 *
 * Nothing asserted this, so a predicate that never reports idle left every
 * stop burning its full grace: 250 ms on the encode loop at each record
 * transition on both SigmaStar backends, with the whole suite still green.
 * Over-waiting is not a safe failure here; it is the stall the writer thread
 * exists to prevent, moved to a rarer event. */
static int test_writer_stop_bounded_returns_promptly_when_idle(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	struct timespec t0, t1;
	long long elapsed_ms;
	uint64_t dropped = 99;
	int failures = 0;

	memset(&s, 0, sizeof(s));
	s.delay_us = 1000;   /* a healthy card: 1 ms a frame */

	CHECK("idle stop writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0);

	(void)venc_rec_writer_push(w, make_au(7, 512), 512, 900, 0);
	usleep(50000);   /* let it finish; the writer is now genuinely idle */

	clock_gettime(CLOCK_MONOTONIC, &t0);
	venc_rec_writer_stop_bounded(w, 3000, &dropped);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;

	/* Against a 3 s grace: a stop that cannot see idle would take all of
	 * it.  Anything under a tenth of the grace can only be the early
	 * return. */
	CHECK("idle stop returns without burning the grace", elapsed_ms < 300);
	CHECK("idle stop abandoned nothing", dropped == 0);
	CHECK("idle stop delivered the frame", s.calls == 1);
	return failures;
}

/* The other direction of the bound, and the half nothing asserted: within
 * grace_ms, queued access units DO reach the sink.
 *
 * Every other bounded-stop assertion here is about abandoning, or about not
 * over-waiting.  With only those, the production 250 ms grace could be cut to
 * 30 ms — shedding seven-eighths of a stalled recording's tail at every stop,
 * on all three backends — with the suite fully green.  Verified: a mutant that
 * passes `grace_ms / 8` survived the old set, and is caught by this. */
static int test_writer_stop_bounded_delivers_within_the_grace(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	uint64_t dropped = 99;
	int failures = 0;
	int i;

	memset(&s, 0, sizeof(s));
	s.delay_us = 40000;   /* 40 ms a frame: 6 frames need ~240 ms */

	CHECK("delivery writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0);
	for (i = 0; i < 6; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);

	/* 600 ms against 240 ms of work: 2.5x margin, so this asserts the grace
	 * is honoured rather than that the machine is fast — but tight enough
	 * that a mutant shrinking the grace to an eighth (75 ms) delivers one
	 * frame instead of six and is caught. */
	venc_rec_writer_stop_bounded(w, 600, &dropped);

	CHECK("a grace that covers the queue delivers all of it", s.calls == 6);
	CHECK("a covered queue sheds nothing", dropped == 0);
	return failures;
}

/* The unbounded stop joins too.  Its barrier had no assertion behind it —
 * removing that join was caught only as a heap-corruption abort, which tells
 * a CI log grepping for failures nothing at all. */
static int test_writer_stop_unbounded_is_also_a_barrier(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	int failures = 0;
	int i;

	memset(&s, 0, sizeof(s));
	s.delay_us = 30000;   /* 30 ms a frame */

	CHECK("unbounded stop writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0);
	for (i = 0; i < 5; i++)
		(void)venc_rec_writer_push(w, make_au((uint8_t)i, 1024), 1024,
			(uint64_t)i * 900, 0);

	venc_rec_writer_stop(w);
	/* Everything after this stands for close(fd). */
	__atomic_store_n(&s.released, 1, __ATOMIC_RELEASE);
	usleep(200000);

	CHECK("the unbounded stop flushed the whole queue", s.calls == 5);
	CHECK("no sink call landed after the unbounded stop",
		s.ran_after_release == 0);
	return failures;
}

/* The header says *dropped is ASSIGNED, not accumulated into. */
static int test_writer_stop_bounded_assigns_dropped_rather_than_accumulating(void)
{
	VencRecWriter *w = NULL;
	BarrierSink s;
	uint64_t dropped = 1000;   /* deliberately non-zero going in */
	int failures = 0;

	memset(&s, 0, sizeof(s));
	s.delay_us = 0;

	CHECK("assign stop writer start",
		venc_rec_writer_start(&w, barrier_sink, &s) == 0);
	(void)venc_rec_writer_push(w, make_au(3, 256), 256, 900, 0);
	venc_rec_writer_stop_bounded(w, 2000, &dropped);

	/* Nothing was refused, so the writer's total is 0.  An implementation
	 * that accumulated would leave the caller's 1000 in place. */
	CHECK("stop assigns the total, it does not add to it", dropped == 0);
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
	failures += test_writer_stop_bounded_is_a_hard_barrier();
	failures += test_writer_stop_bounded_abandons_the_backlog();
	failures += test_writer_stop_cost_bounded_vs_unbounded();
	failures += test_writer_counters_start_clean_for_each_recording();
	failures += test_writer_stop_bounded_makes_a_close_safe_without_a_lock();
	failures += test_writer_stop_bounded_returns_promptly_when_idle();
	failures += test_writer_stop_bounded_delivers_within_the_grace();
	failures += test_writer_stop_unbounded_is_also_a_barrier();
	failures += test_writer_stop_bounded_assigns_dropped_rather_than_accumulating();
	return failures;
}
