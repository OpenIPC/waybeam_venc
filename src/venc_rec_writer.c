#include "venc_rec_writer.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Intrusive singly-linked FIFO of heap buffers.  A linked list rather than a
 * ring: entries are already individually malloc'd (the drain loop hands over
 * the buffer it built), so a ring would add a fixed-size index array without
 * removing an allocation. */
typedef struct VencRecAu {
	struct VencRecAu *next;
	uint8_t          *au;
	size_t            len;
	uint64_t          pts_90khz;
	int               is_idr;
} VencRecAu;

struct VencRecWriter {
	pthread_mutex_t  lock;
	pthread_cond_t   cond;
	pthread_t        thread;
	bool             thread_started;
	bool             stopping;

	/* True from the moment an entry leaves the queue until its sink call
	 * has returned.  The queue is popped UNDER the lock but the sink runs
	 * OUTSIDE it, so `head == NULL` says nothing about whether a write is
	 * still in progress — a drain that tested only the queue would return
	 * while the sink still held the caller's fd.  The flag closes exactly
	 * that window. */
	bool             in_flight;

	VencRecAu       *head;
	VencRecAu       *tail;
	uint32_t         depth;
	uint32_t         peak_depth;
	size_t           bytes;

	uint64_t         queued;
	uint64_t         dropped;

	VencRecSinkFn    sink;
	void            *ctx;
};

static void au_free(VencRecAu *e)
{
	if (!e)
		return;
	free(e->au);
	free(e);
}

/* Caller holds the lock.  Returns NULL when the queue is empty. */
static VencRecAu *queue_pop(VencRecWriter *w)
{
	VencRecAu *e = w->head;

	if (!e)
		return NULL;
	w->head = e->next;
	if (!w->head)
		w->tail = NULL;
	w->depth--;
	w->bytes -= e->len;
	e->next = NULL;
	return e;
}

static void *writer_thread(void *opaque)
{
	VencRecWriter *w = opaque;

	pthread_mutex_lock(&w->lock);
	for (;;) {
		VencRecAu *e = queue_pop(w);

		if (!e) {
			/* Drain before honouring the stop: venc_rec_writer_stop()
			 * promises everything already queued reaches the file. */
			if (w->stopping)
				break;
			pthread_cond_wait(&w->cond, &w->lock);
			continue;
		}

		/* The sink blocks on the disk — that is the entire point of this
		 * thread — so it must not hold the lock, or push() would block
		 * with it and the stall would reach the drain loop anyway.
		 * Publish `in_flight` before dropping the lock so a waiter can
		 * never observe the gap between the pop and the write. */
		w->in_flight = true;
		pthread_mutex_unlock(&w->lock);
		if (w->sink)
			w->sink(w->ctx, e->au, e->len, e->pts_90khz, e->is_idr);
		au_free(e);
		pthread_mutex_lock(&w->lock);
		w->in_flight = false;
	}
	pthread_mutex_unlock(&w->lock);
	return NULL;
}

int venc_rec_writer_start(VencRecWriter **out, VencRecSinkFn sink, void *ctx)
{
	VencRecWriter *w;
	pthread_attr_t attr;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!sink)
		return -EINVAL;

	w = calloc(1, sizeof(*w));
	if (!w)
		return -ENOMEM;
	w->sink = sink;
	w->ctx = ctx;

	if (pthread_mutex_init(&w->lock, NULL) != 0) {
		free(w);
		return -EIO;
	}
	if (pthread_cond_init(&w->cond, NULL) != 0) {
		pthread_mutex_destroy(&w->lock);
		free(w);
		return -EIO;
	}
	/* The stack size is NOT incidental.  The sink runs the TS muxer, and
	 * star6e_ts_recorder_write_video() declares ts_buf[3000 * 188] — a
	 * 551 KB automatic array.  On glibc that fits the 8 MB default and on
	 * the main thread it always did; musl (every SigmaStar and CV610
	 * target) gives a new pthread only 128 KB, so the very first recorded
	 * frame overflows the stack and the process dies.  Device-reproduced
	 * on CV610: venc exited immediately after "[ts_recorder] started",
	 * with the host suite green because it links glibc.
	 *
	 * 1 MB covers that buffer plus the mux frames above it, and costs
	 * nothing while idle — Linux commits thread stacks lazily. */
	if (pthread_attr_init(&attr) != 0) {
		pthread_cond_destroy(&w->cond);
		pthread_mutex_destroy(&w->lock);
		free(w);
		return -EIO;
	}
	(void)pthread_attr_setstacksize(&attr, VENC_REC_WRITER_STACK_BYTES);
	if (pthread_create(&w->thread, &attr, writer_thread, w) != 0) {
		fprintf(stderr, "[rec_writer] thread create failed: %s\n",
			strerror(errno));
		pthread_attr_destroy(&attr);
		pthread_cond_destroy(&w->cond);
		pthread_mutex_destroy(&w->lock);
		free(w);
		return -EIO;
	}
	pthread_attr_destroy(&attr);
	w->thread_started = true;

	*out = w;
	return 0;
}

int venc_rec_writer_push(VencRecWriter *w, uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	VencRecAu *e;

	/* Ownership is taken unconditionally, including on every rejection
	 * path, so a caller can hand the buffer over and forget it. */
	if (!w || !au || len == 0) {
		free(au);
		return 0;
	}

	e = malloc(sizeof(*e));
	if (!e) {
		free(au);
		pthread_mutex_lock(&w->lock);
		w->dropped++;
		pthread_mutex_unlock(&w->lock);
		return 0;
	}
	e->next = NULL;
	e->au = au;
	e->len = len;
	e->pts_90khz = pts_90khz;
	e->is_idr = is_idr;

	pthread_mutex_lock(&w->lock);
	if (w->stopping || w->bytes + len > VENC_REC_WRITER_MAX_BYTES) {
		/* Drop the INCOMING frame, not the oldest.  Dropping the oldest
		 * would reorder the file and, worse, could discard the IRAP the
		 * recording opened with while keeping the frames that reference
		 * it.  A tail drop leaves a shorter but coherent prefix. */
		w->dropped++;
		pthread_mutex_unlock(&w->lock);
		au_free(e);
		return 0;
	}

	if (w->tail)
		w->tail->next = e;
	else
		w->head = e;
	w->tail = e;
	w->depth++;
	w->bytes += len;
	w->queued++;
	if (w->depth > w->peak_depth)
		w->peak_depth = w->depth;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);
	return 1;
}

/* Common tail: the thread has exited, so nothing else can touch the queue. */
static void writer_destroy(VencRecWriter *w, uint64_t *dropped)
{
	for (;;) {
		VencRecAu *e = queue_pop(w);

		if (!e)
			break;
		w->dropped++;
		au_free(e);
	}
	if (dropped)
		*dropped = w->dropped;

	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->lock);
	free(w);
}

/* Wait until the writer thread is genuinely idle: nothing queued AND nothing
 * being written.  Returns 1 if it reached that state, 0 on timeout.
 * Caller must NOT hold the lock. */
static int queue_wait_empty(VencRecWriter *w, unsigned grace_ms)
{
	unsigned waited = 0;

	for (;;) {
		int done;

		pthread_mutex_lock(&w->lock);
		done = (w->head == NULL && !w->in_flight);
		pthread_mutex_unlock(&w->lock);
		if (done)
			return 1;
		if (waited >= grace_ms)
			return 0;
		usleep(2000);
		waited += 2;
	}
}

void venc_rec_writer_drain(VencRecWriter *w, unsigned grace_ms,
	uint64_t *dropped)
{
	if (!w)
		return;

	if (!queue_wait_empty(w, grace_ms)) {
		/* Timed out: abandon the queue rather than hold the encode loop
		 * for a stalled card.  An access unit already in the sink's hands
		 * is NOT waited for here — bounding this path is the whole point
		 * — so the caller must serialise its own teardown against the
		 * sink.  Both SigmaStar backends do that with rec_state_lock. */
		pthread_mutex_lock(&w->lock);
		for (;;) {
			VencRecAu *e = queue_pop(w);

			if (!e)
				break;
			w->dropped++;
			au_free(e);
		}
		pthread_mutex_unlock(&w->lock);
	}
	if (dropped) {
		pthread_mutex_lock(&w->lock);
		*dropped = w->dropped;
		pthread_mutex_unlock(&w->lock);
	}
}

void venc_rec_writer_stop_bounded(VencRecWriter *w, unsigned grace_ms,
	uint64_t *dropped)
{
	if (!w)
		return;

	pthread_mutex_lock(&w->lock);
	w->stopping = true;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);

	if (!w->thread_started) {
		writer_destroy(w, dropped);
		return;
	}

	/* Poll rather than pthread_timedjoin_np: musl does not have it. */
	(void)queue_wait_empty(w, grace_ms);

	/* Whether it drained or timed out, `stopping` is set, so the thread
	 * refuses new work and exits as soon as it finishes the AU in its
	 * hand.  The join therefore waits for at most ONE sink call, not the
	 * whole backlog; the rest is abandoned below and counted. */
	pthread_join(w->thread, NULL);
	writer_destroy(w, dropped);
}

void venc_rec_writer_stop(VencRecWriter *w)
{
	if (!w)
		return;

	pthread_mutex_lock(&w->lock);
	w->stopping = true;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);

	if (w->thread_started)
		pthread_join(w->thread, NULL);

	writer_destroy(w, NULL);
}

void venc_rec_writer_reset_counters(VencRecWriter *w)
{
	if (!w)
		return;
	pthread_mutex_lock(&w->lock);
	w->queued = 0;
	w->dropped = 0;
	w->peak_depth = w->depth;
	pthread_mutex_unlock(&w->lock);
}

void venc_rec_writer_stats(const VencRecWriter *w, uint64_t *queued,
	uint64_t *dropped, uint32_t *depth, uint32_t *peak_depth)
{
	VencRecWriter *mw = (VencRecWriter *)w;

	if (!w) {
		if (queued) *queued = 0;
		if (dropped) *dropped = 0;
		if (depth) *depth = 0;
		if (peak_depth) *peak_depth = 0;
		return;
	}

	pthread_mutex_lock(&mw->lock);
	if (queued) *queued = mw->queued;
	if (dropped) *dropped = mw->dropped;
	if (depth) *depth = mw->depth;
	if (peak_depth) *peak_depth = mw->peak_depth;
	pthread_mutex_unlock(&mw->lock);
}
