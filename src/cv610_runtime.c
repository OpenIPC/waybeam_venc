#include "cv610_runtime.h"

#include "cv610_audio.h"
#include "cv610_encoder_config.h"
#include "pipeline_common.h"
#include "cv610_iq.h"
#include "cv610_modes.h"
#include "cv610_pipeline.h"
#include "debug_osd.h"
#include "h26x_param_sets.h"
#include "h26x_util.h"
#include "hevc_rtp.h"
#include "idr_rate_limit.h"
#include "audio_ring.h"
#include "star6e_recorder.h"
#include "star6e_ts_recorder.h"
#include "venc_jpeg.h"
#include "venc_rec_writer.h"
#include "output_socket.h"
#include "rtp_session.h"
#include "timing.h"
#include "venc_frame_ring.h"
#include "venc_ring.h"
#include "venc_api.h"
#include "venc_httpd.h"
#include "venc_respawn.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "ot_common.h"
#include "ot_common_sys.h"
#include "ot_common_venc.h"
#include "ss_mpi_sys_bind.h"
#include "ss_mpi_venc.h"

#define CV610_VPSS_GRP 0
#define CV610_VPSS_CHN 0
#define CV610_VENC_CHN 0
#define CV610_FRAME_RING_SLOTS 8
#define CV610_FRAME_RING_BYTES (512u * 1024u)

typedef struct {
	VencConfig config;
	Cv610PipelineConfig pipeline;
	VencOutputUri output_uri;
	int socket_handle;
	struct sockaddr_storage destination;
	socklen_t destination_len;
	VencOutputUriType transport;
	int connected_udp;
	OutputSocketQueue send_queue;
	venc_frame_ring_t *frame_ring;
	/* frame-shm ring low-water measurement.  Every frame-shm producer must
	 * measure: venc_frame_ring_create() publishes the VHLT health marker
	 * unconditionally, so a ring nobody measures still advertises a live
	 * gauge sitting at its create-time 0 -- the healthiest value in the
	 * range.  "Not measured" would be indistinguishable from "the consumer
	 * is keeping up perfectly", to the rate controller that is now solely
	 * responsible for reacting. */
	VencRingLowWater low_water;
	int low_water_ready;
	RtpPacketizerState rtp;
	H26xParamSets param_sets;
	DebugOsdState *debug_osd;
	Cv610AudioState *audio;
	HevcRtpStats rtp_stats;
	uint32_t frame_ticks;
	uint32_t live_bitrate;
	uint16_t max_payload_size;
	int verbose;
	uint64_t frames;
	uint64_t bytes;
	uint64_t output_drops;
	uint64_t packets_sent;
	uint8_t gdr_active;
	uint8_t gdr_cycle_len;
	uint8_t gdr_counter;
	uint8_t svct_active;
	uint8_t delivered_slice_count;
	uint8_t slice_census_done;
	uint32_t requested_slice_count;
	uint32_t intra_refresh_active;
	uint32_t applied_gop_frames;
	uint32_t ref_census_frames;
	uint32_t ref_type_counts[OT_VENC_P_SLICE_BUTT];
	uint32_t trail_n_patched;
	int venc_created;
	int venc_started;
	int venc_bound;
	/* Recording (record.mode=mirror only on this backend): the main
	 * channel's already-copied access unit is teed to file.  The shared
	 * recorder cores are SoC-independent — CV610 needs no SDK-typed
	 * adapter because cv610_copy_stream() has already produced one
	 * contiguous Annex-B buffer by the time either writer sees it. */
	Star6eRecorderState recorder;
	Star6eTsRecorderState ts_recorder;
	AudioRing audio_ring;
	/* Disk writes run here, not on the encode drain loop.  A blocking
	 * write(2) between the transport send and release_stream() stalls the
	 * LIVE stream, device-measured at 22% of normal throughput for a full
	 * second on a marginal card (see include/venc_rec_writer.h). */
	VencRecWriter *rec_writer;
	/* rec_writer is created and freed on the encode loop but READ by the
	 * httpd thread for /api/v1/record/status, so the pointer itself needs
	 * a lock: without one, a status poll racing a record/stop dereferences
	 * a freed writer, and the stop blocks in pthread_join for as long as
	 * the queue takes to flush — seconds on the stalled disk this exists
	 * to survive. */
	pthread_mutex_t rec_writer_lock;
	/* rec_writer_lock and both recorders are initialised partway through
	 * cv610_init(), which can fail before reaching them — and backend.c
	 * calls the teardown regardless.  Without this, that teardown locks a
	 * never-initialised mutex and calls star6e_ts_recorder_stop() on a
	 * calloc'd state whose fd is 0, which passes the `fd < 0` test and
	 * fdatasync/close()es STDIN.  star6e and maruko carry the same flag. */
	int rec_locks_ready;
	/* Kept in the context, not the writer: the counters have to outlive
	 * the writer or a recording that shed frames reports droppedFrames:0
	 * the moment it stops — exactly the silent-damage case the counter was
	 * added to prevent. */
	uint64_t rec_dropped_frames;
	uint32_t rec_writer_peak_depth;
} Cv610RunnerContext;

/* Backend selection and the vendor MPP graph are process-singleton. The
 * shared VencApplyCallbacks ABI carries no opaque pointer, so—like the
 * Star6E and Maruko runtimes—the live-control wrappers refer to the one active
 * runner. It is published before HTTP starts and cleared after HTTP joins. */
static Cv610RunnerContext *g_cv610_runner;

static Cv610IntraRefreshStatus g_cv610_intra_status;
static Cv610RefPredStatus g_cv610_ref_status;
static pthread_mutex_t g_cv610_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void cv610_runtime_intra_refresh_status(Cv610IntraRefreshStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_cv610_status_mutex);
	*out = g_cv610_intra_status;
	pthread_mutex_unlock(&g_cv610_status_mutex);
}

void cv610_runtime_ref_pred_status(Cv610RefPredStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_cv610_status_mutex);
	*out = g_cv610_ref_status;
	pthread_mutex_unlock(&g_cv610_status_mutex);
}

static void cv610_publish_encoder_status(
	const Cv610IntraRefreshStatus *intra,
	const Cv610RefPredStatus *ref)
{
	pthread_mutex_lock(&g_cv610_status_mutex);
	if (intra)
		g_cv610_intra_status = *intra;
	if (ref)
		g_cv610_ref_status = *ref;
	pthread_mutex_unlock(&g_cv610_status_mutex);
}

static int cv610_update_venc_attr(uint32_t bitrate, uint32_t gop,
	int qp_delta, unsigned int fields)
{
	ot_venc_chn_attr attr;
	td_s32 ret;

	memset(&attr, 0, sizeof(attr));
	ret = ss_mpi_venc_get_chn_attr(CV610_VENC_CHN, &attr);
	if (ret != TD_SUCCESS || attr.rc_attr.rc_mode != OT_VENC_RC_MODE_H265_CBR)
		return -1;
	if (fields & 1u)
		attr.rc_attr.h265_cbr.bit_rate = bitrate;
	if (fields & 2u)
		attr.rc_attr.h265_cbr.gop = gop;
	if (fields & 4u)
		attr.gop_attr.normal_p.ip_qp_delta = qp_delta;
	ret = ss_mpi_venc_set_chn_attr(CV610_VENC_CHN, &attr);
	return ret == TD_SUCCESS ? 0 : -1;
}

static int cv610_apply_bitrate(uint32_t kbps)
{
	int ret = cv610_update_venc_attr(kbps, 0, 0, 1u);

	if (ret == 0 && g_cv610_runner)
		__atomic_store_n(&g_cv610_runner->live_bitrate, kbps,
			__ATOMIC_RELEASE);
	return ret;
}

/* The CV610 encoder cannot take a GOP change while intra refresh is running.
 * Measured on .181 2026-08-23: writing attr.rc_attr.h265_cbr.gop resets the
 * channel's intra-refresh state, converting a GDR stream (recovery_point SEI,
 * no IRAP) into an IDR stream for the rest of this venc lifetime -- and writing
 * the old value back does not undo it, only a restart does.  Re-asserting intra
 * refresh right after the write does restore GDR, but the recovery period stays
 * where it was (identical at gop 0.5/1.0/2.0/4.0) while the CBR window still
 * moves, swinging the achieved rate 2.7..11.2 Mbps against a fixed 9.26 Mbps
 * target.  The write can therefore be neither honoured nor made harmless, so
 * refuse it and leave the encoder untouched.
 *
 * This is not a general venc rule: with resilience "off" intra refresh is
 * inactive, video0.gop_size is the operator's to set (venc_config.c: a named
 * preset owns gop_size, only "off" preserves the user's value) and the write
 * proceeds normally.  Star6E is unaffected and keeps live GOP control
 * (device-verified on .232: cadence moved 120 -> 60 frames, GDR intact). */
static int cv610_apply_gop(uint32_t frames)
{
	Cv610RunnerContext *ctx = g_cv610_runner;
	int ret;

	/* No runner means no channel to write to. Refuse rather than fall through
	 * to the write, matching cv610_apply_verbose()/cv610_apply_max_payload_size();
	 * falling through would perform exactly the write this function exists to
	 * prevent. */
	if (!ctx)
		return -1;

	if (ctx->intra_refresh_active) {
		/* Accept a write that asks for what is already in force.  When a
		 * later group in the same request fails, the API replays the
		 * previous value of every applied group as a rollback step;
		 * refusing that replay would report "rollback incomplete" against
		 * a channel nothing had touched, which reads as damage. */
		if (frames == ctx->applied_gop_frames)
			return 0;
		fprintf(stderr, "WARNING: CV610 refusing live gop=%u: the GOP is "
			"owned by intra refresh (resilience preset); applying it "
			"would drop the stream out of GDR\n", (unsigned)frames);
		return -1;
	}
	ret = cv610_update_venc_attr(0, frames, 0, 2u);
	if (ret == 0)
		ctx->applied_gop_frames = frames;
	return ret;
}

static int cv610_apply_qp_delta(int delta)
{
	return cv610_update_venc_attr(0, 0, delta, 4u);
}

/* Captured on the first write so that 0 restores the driver's own default
 * rather than pinning whatever happened to be in force at the time. */
static struct {
	td_u32 max_qp, min_qp, max_i_qp, min_i_qp;
	int captured;
} g_cv610_qp_defaults;

/* CBR can only hold its target by raising QP.  In a high-gain, noise-dominated
 * scene the QP the encoder needs can exceed the driver's default ceiling, and
 * once it saturates the bitrate overshoots with the rate controller powerless
 * — measured at 3-7x target on the .181 bench with the lights off.  This is
 * the knob that lets it squeeze; it does not make the picture better, it stops
 * the overshoot.  0 on either bound leaves/restores the driver default. */
static int cv610_apply_qp_bounds(uint32_t min_qp, uint32_t max_qp)
{
	ot_venc_chn_attr attr;
	ot_venc_rc_param param;
	td_s32 ret;

	if (min_qp == 0 && max_qp == 0 && !g_cv610_qp_defaults.captured)
		return 0;   /* never written — driver defaults already in force */

	memset(&attr, 0, sizeof(attr));
	if (ss_mpi_venc_get_chn_attr(CV610_VENC_CHN, &attr) != TD_SUCCESS)
		return -1;
	/* Only CBR carries these bounds; the backend configures nothing else,
	 * but a future mode change must not write through the wrong union arm. */
	if (attr.rc_attr.rc_mode != OT_VENC_RC_MODE_H265_CBR)
		return -1;

	memset(&param, 0, sizeof(param));
	if (ss_mpi_venc_get_rc_param(CV610_VENC_CHN, &param) != TD_SUCCESS)
		return -1;

	if (!g_cv610_qp_defaults.captured) {
		g_cv610_qp_defaults.max_qp   = param.h265_cbr_param.max_qp;
		g_cv610_qp_defaults.min_qp   = param.h265_cbr_param.min_qp;
		g_cv610_qp_defaults.max_i_qp = param.h265_cbr_param.max_i_qp;
		g_cv610_qp_defaults.min_i_qp = param.h265_cbr_param.min_i_qp;
		g_cv610_qp_defaults.captured = 1;
	}

	param.h265_cbr_param.max_qp = max_qp ? max_qp : g_cv610_qp_defaults.max_qp;
	param.h265_cbr_param.min_qp = min_qp ? min_qp : g_cv610_qp_defaults.min_qp;
	/* I-frames get the same CEILING -- raising only the P bound leaves the
	 * I-frame free to blow the budget on its own, which in a noisy scene is
	 * where the biggest frames come from.  Their FLOOR is deliberately left
	 * alone: video0.qp_delta biases I-frames below the P QP (this craft ships
	 * -4), and an I-frame floor would silently cancel it.  Star6E's
	 * apply_qp_bounds() touches only the P bounds for the same reason. */
	param.h265_cbr_param.max_i_qp = max_qp ? max_qp : g_cv610_qp_defaults.max_i_qp;

	/* The API validator only compares min against max when BOTH are non-zero,
	 * so a half-specified pair can still resolve to min > max against the
	 * driver default.  The SDK takes that without complaint and then behaves
	 * erratically, so reject it here rather than write it. */
	if (param.h265_cbr_param.min_qp > param.h265_cbr_param.max_qp ||
		param.h265_cbr_param.min_i_qp > param.h265_cbr_param.max_i_qp) {
		fprintf(stderr, "ERROR: qpBounds min>max after resolving defaults "
			"(p %u/%u, i %u/%u)\n",
			(unsigned)param.h265_cbr_param.min_qp,
			(unsigned)param.h265_cbr_param.max_qp,
			(unsigned)param.h265_cbr_param.min_i_qp,
			(unsigned)param.h265_cbr_param.max_i_qp);
		return -1;
	}

	ret = ss_mpi_venc_set_rc_param(CV610_VENC_CHN, &param);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_set_rc_param=0x%x\n",
			(unsigned)ret);
		return -1;
	}
	printf("> qpBounds: min=%u max=%u (0 = driver default %u/%u)\n",
		(unsigned)param.h265_cbr_param.min_qp,
		(unsigned)param.h265_cbr_param.max_qp,
		(unsigned)g_cv610_qp_defaults.min_qp,
		(unsigned)g_cv610_qp_defaults.max_qp);
	return 0;
}

static int cv610_apply_verbose(bool on)
{
	if (!g_cv610_runner)
		return -1;
	__atomic_store_n(&g_cv610_runner->verbose, on ? 1 : 0,
		__ATOMIC_RELEASE);
	return 0;
}

static int cv610_apply_max_payload_size(uint16_t size)
{
	Cv610RunnerContext *ctx = g_cv610_runner;

	if (!ctx)
		return -1;
	__atomic_store_n(&ctx->max_payload_size, size, __ATOMIC_RELEASE);
	return 0;
}

static int cv610_request_idr(void)
{
	if (!idr_rate_limit_allow(CV610_VENC_CHN))
		return 0;
	return ss_mpi_venc_request_idr(CV610_VENC_CHN, TD_TRUE) == TD_SUCCESS
		? 0 : -1;
}

/* Recorder start is a BOOTSTRAP event, not a request for a fresher picture.
 * The file that just opened contains nothing, and the shipped CV610 config is
 * resilience=racing — a GDR craft emits no periodic IDR at all, so a request
 * that the rate limiter coalesces away leaves a recording with NO IRAP access
 * unit anywhere in it.  That file seeks to nothing and plays from nothing
 * while the caller was told the start succeeded.  Same reasoning and same
 * un-coalescible path as Star6E's runtime_request_idr_on(). */
static void cv610_record_force_idr(void)
{
	idr_rate_limit_force(CV610_VENC_CHN);
	if (ss_mpi_venc_request_idr(CV610_VENC_CHN, TD_TRUE) != TD_SUCCESS)
		fprintf(stderr, "WARN: record start could not force an IDR; "
			"the recording may not decode from its first frame\n");
}

/* Runs on the writer thread, one access unit at a time, in order.  Both
 * writers no-op while their recorder is closed, and the writer is always
 * stopped before either is, so this cannot race a close. */
static void cv610_record_sink(void *opaque, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr)
{
	Cv610RunnerContext *ctx = opaque;

	if (star6e_ts_recorder_is_active(&ctx->ts_recorder))
		(void)star6e_ts_recorder_write_video(&ctx->ts_recorder, au, len,
			pts_90khz, is_idr);
	else
		(void)star6e_recorder_write_au(&ctx->recorder, au, len);
}

/* Close whichever recorder is open and drop the audio tee.  Idempotent.
 *
 * Order matters: the writer thread is the only thread that touches recorder
 * state while recording, so it has to be drained and joined BEFORE either
 * recorder is closed. */
/* `bounded` picks how long the queued tail may hold this caller up.
 *
 * Mid-run (the encode loop) it must be bounded: an unbounded drain would put
 * the disk stall straight back on the live video path at every stop, which is
 * the whole thing this writer removes.  At teardown the video path is going
 * away regardless, so blocking is free and the full flush is what gets the
 * recording's last seconds onto disk instead of discarding them. */
static void cv610_record_stop(Cv610RunnerContext *ctx, int bounded)
{
	VencRecWriter *w;

	/* An init that failed before the recorders and the mutex were set up
	 * leaves them zeroed; locking one of those is undefined, and a zeroed
	 * recorder fd of 0 reads as an open file. */
	if (!ctx->rec_locks_ready)
		return;

	/* Detach under the lock so a status poll already inside
	 * venc_rec_writer_stats() finishes against a live writer, and any
	 * later one sees NULL.  Harvest the counters first — they must
	 * survive the writer they came from. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	w = ctx->rec_writer;
	if (w) {
		uint64_t dropped = 0;
		uint32_t peak = 0;

		venc_rec_writer_stats(w, NULL, &dropped, NULL, &peak);
		ctx->rec_dropped_frames = dropped;
		ctx->rec_writer_peak_depth = peak;
	}
	ctx->rec_writer = NULL;
	pthread_mutex_unlock(&ctx->rec_writer_lock);

	/* Bounded, and outside the lock.  An unbounded drain would put the
	 * disk stall back on the encode loop at every stop — the very thing
	 * the writer removes — so a stalled queue is abandoned instead: the
	 * recording loses its tail, the live link loses nothing. */
	if (bounded) {
		uint64_t dropped = 0;

		venc_rec_writer_stop_bounded(w, 250, &dropped);
		/* Only if there WAS a writer: stop_bounded leaves *dropped at 0
		 * for a NULL one, which would clobber the harvest above.  Stored
		 * under the lock because the httpd thread reads it and an
		 * unlocked 64-bit store tears on ARM32. */
		if (w) {
			pthread_mutex_lock(&ctx->rec_writer_lock);
			ctx->rec_dropped_frames = dropped;
			pthread_mutex_unlock(&ctx->rec_writer_lock);
		}
	} else {
		venc_rec_writer_stop(w);
	}
	cv610_audio_set_record_ring(ctx->audio, NULL);
	star6e_ts_recorder_stop(&ctx->ts_recorder);
	star6e_recorder_stop(&ctx->recorder);
}

static void cv610_record_start(Cv610RunnerContext *ctx, const char *dir)
{
	int want_audio;
	int rc;

	if (!dir || !dir[0] || !ctx->rec_locks_ready)
		return;
	/* Stop first: a start over a live recording must not leak the open fd,
	 * and only one of the two recorders may ever be active. */
	cv610_record_stop(ctx, 1);

	/* Before the open, not after it: these describe the recording being
	 * started, and a start that FAILS must not leave the previous
	 * recording's drop count standing as if it belonged to this one. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	ctx->rec_dropped_frames = 0;
	ctx->rec_writer_peak_depth = 0;
	pthread_mutex_unlock(&ctx->rec_writer_lock);

	if (strcmp(ctx->config.record.format, "hevc") == 0) {
		if (star6e_recorder_start(&ctx->recorder, dir) != 0)
			return;
	} else {
		want_audio = ctx->config.audio.enabled && ctx->audio;
		if (star6e_ts_recorder_start(&ctx->ts_recorder, dir,
				want_audio ? &ctx->audio_ring : NULL) != 0)
			return;
		if (want_audio)
			cv610_audio_set_record_ring(ctx->audio, &ctx->audio_ring);
	}

	/* After the file is open, so the sink never sees a closed recorder.
	 *
	 * A failed start CLOSES the recording rather than leaving it open: the
	 * drain loop only pushes when rec_writer is non-NULL, so a NULL writer
	 * with an open file reports active:true with framesWritten frozen at 0
	 * forever and a file holding nothing but PAT/PMT — a phantom recording,
	 * which is worse than an honest failure. */
	pthread_mutex_lock(&ctx->rec_writer_lock);
	rc = venc_rec_writer_start(&ctx->rec_writer, cv610_record_sink, ctx);
	pthread_mutex_unlock(&ctx->rec_writer_lock);
	if (rc != 0) {
		fprintf(stderr, "ERROR: recorder writer thread did not start "
			"(%d); recording not started\n", rc);
		cv610_record_stop(ctx, 1);
		return;
	}

	cv610_record_force_idr();
}

static void cv610_record_status_callback(VencRecordStatus *out)
{
	Cv610RunnerContext *ctx = g_cv610_runner;

	memset(out, 0, sizeof(*out));
	if (!ctx)
		return;

	{
		uint64_t dropped;
		uint32_t peak;

		/* Under the lock: cv610_record_stop() frees the writer from the
		 * encode loop, and this runs on the httpd thread.
		 *
		 * The stored fallbacks are read inside it too, not just the
		 * writer: a 64-bit load on ARM32 is two instructions and can
		 * straddle the encode loop's store as easily as the store can
		 * tear.  star6e and maruko already do it this way. */
		pthread_mutex_lock(&ctx->rec_writer_lock);
		dropped = ctx->rec_dropped_frames;
		peak = ctx->rec_writer_peak_depth;
		if (ctx->rec_writer)
			venc_rec_writer_stats(ctx->rec_writer, NULL, &dropped,
				NULL, &peak);
		pthread_mutex_unlock(&ctx->rec_writer_lock);
		out->dropped_frames = (uint32_t)dropped;
		out->writer_peak_depth = peak;
	}

	/* is_RECORDING, not is_active: rotation runs on the writer thread here
	 * too (see the take_idr_request hand-off in the drain loop) and holds
	 * fd == -1 across it, so the descriptor is not the right question for a
	 * reader on the httpd thread. */
	if (star6e_ts_recorder_is_recording(&ctx->ts_recorder)) {
		out->active = 1;
		snprintf(out->format, sizeof(out->format), "ts");
		star6e_ts_recorder_status(&ctx->ts_recorder,
			&out->bytes_written, &out->frames_written,
			&out->segments, NULL, NULL);
		snprintf(out->path, sizeof(out->path), "%s", ctx->ts_recorder.path);
		out->elapsed_ms =
			star6e_recorder_elapsed_ms(&ctx->ts_recorder.start_time);
		snprintf(out->stop_reason, sizeof(out->stop_reason), "none");
	} else if (star6e_recorder_is_active(&ctx->recorder)) {
		out->active = 1;
		snprintf(out->format, sizeof(out->format), "hevc");
		star6e_recorder_status(&ctx->recorder,
			&out->bytes_written, &out->frames_written, NULL, NULL);
		snprintf(out->path, sizeof(out->path), "%s", ctx->recorder.path);
		out->elapsed_ms =
			star6e_recorder_elapsed_ms(&ctx->recorder.start_time);
		snprintf(out->stop_reason, sizeof(out->stop_reason), "none");
	} else {
		/* Either recorder may hold the reason; a manual stop on one does
		 * not mask a disk-full on the other. */
		const char *reason = "manual";
		Star6eRecorderStopReason sr = ctx->ts_recorder.last_stop_reason;

		if (sr == RECORDER_STOP_MANUAL)
			sr = ctx->recorder.last_stop_reason;
		if (sr == RECORDER_STOP_DISK_FULL)
			reason = "disk_full";
		else if (sr == RECORDER_STOP_WRITE_ERROR)
			reason = "write_error";
		snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", reason);
		snprintf(out->format, sizeof(out->format), "%s",
			ctx->config.record.format);
	}
}

/* Star6E/Maruko parity — see star6e_service_ring_low_water().  venc measures
 * egress pressure and publishes it; it never acts on it. */
static void cv610_service_ring_low_water(Cv610RunnerContext *ctx)
{
	venc_frame_ring_fill_t fill;
	uint64_t now_us;

	if (!ctx)
		return;
	/* Clear the window when there is no ring, exactly as the SigmaStar
	 * backends do: a carried-over low_slots of 0 would swallow the next
	 * ring's first sample and publish "perfectly drained" over a ring
	 * that is full.  Unreachable today (the context is created once per
	 * process), kept symmetric so it stays that way. */
	if (!ctx->frame_ring ||
	    venc_frame_ring_get_fill(ctx->frame_ring, &fill) != 0) {
		venc_ring_low_water_reset(&ctx->low_water, 0);
		ctx->low_water_ready = 0;
		return;
	}

	now_us = wb_monotonic_us();
	if (!ctx->low_water_ready) {
		venc_ring_low_water_reset(&ctx->low_water, now_us);
		ctx->low_water_ready = 1;
	}

	venc_ring_low_water_observe(&ctx->low_water, fill.used_slots,
		fill.slot_count);
	if (venc_ring_low_water_tick(&ctx->low_water, now_us))
		venc_frame_ring_set_low_water(ctx->frame_ring,
			venc_ring_low_water_slots(&ctx->low_water));
}

static uint32_t cv610_query_live_fps(void)
{
	return g_cv610_runner ? g_cv610_runner->pipeline.fps : 0;
}

static char *cv610_query_transport_status(void)
{
	Cv610RunnerContext *ctx = g_cv610_runner;
	char buf[640];
	int pos;
	uint64_t drops;

	if (!ctx)
		return NULL;
	drops = __atomic_load_n(&ctx->output_drops, __ATOMIC_RELAXED);
	if (ctx->frame_ring) {
		venc_frame_ring_fill_t fill;

		if (venc_frame_ring_get_fill(ctx->frame_ring, &fill) != 0)
			return NULL;
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,\"transport\":\"frame-shm\","
			"\"fillPct\":%u,\"inPressure\":%s,"
			"\"transportDrops\":%llu,\"pressureDrops\":0,"
			"\"framesSent\":%llu,\"oversizeDrops\":%llu,"
			"\"slotCount\":%u,\"usedSlots\":%u,"
			"\"ringLowWaterSlots\":%u,\"otherDrops\":%llu}}",
			(unsigned)fill.fill_pct,
			fill.fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT ? "true" : "false",
			(unsigned long long)fill.full_drops,
			(unsigned long long)fill.writes,
			(unsigned long long)fill.oversize_drops,
			(unsigned)fill.slot_count, (unsigned)fill.used_slots,
			(unsigned)venc_ring_low_water_slots(&ctx->low_water),
			(unsigned long long)fill.other_drops);
	} else if (ctx->socket_handle >= 0) {
		uint8_t fill_pct = 0;
		const char *transport = ctx->transport == VENC_OUTPUT_URI_UNIX
			? "unix" : "udp";

		if (output_socket_get_fill_pct(ctx->socket_handle, &ctx->send_queue,
			&fill_pct) != 0)
			fill_pct = 0;
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,\"transport\":\"%s\","
			"\"fillPct\":%u,\"inPressure\":%s,"
			"\"pressureDrops\":0,\"transportDrops\":%llu,"
			"\"packetsSent\":%llu}}",
			transport, (unsigned)fill_pct,
			fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT ? "true" : "false",
			(unsigned long long)drops,
			(unsigned long long)__atomic_load_n(&ctx->packets_sent,
				__ATOMIC_RELAXED));
	} else {
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":false,\"transport\":\"none\"}}");
	}
	if (pos < 0 || pos >= (int)sizeof(buf))
		return NULL;
	return strdup(buf);
}

/* The CV610 audio path is fixed at 48 kHz mono Opus with a hardcoded mic
 * gain (src/cv610_audio.c), so this reports what the hardware is actually
 * doing rather than echoing config fields the encoder never reads.  The
 * counters come from the audio thread under its own stats lock. */
static char *cv610_query_audio_status(void)
{
	Cv610RunnerContext *ctx = g_cv610_runner;
	uint64_t frames = 0, bytes = 0, packets = 0, drops = 0;
	char buf[384];
	int pos;

	if (!ctx)
		return NULL;
	if (!ctx->audio) {
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"enabled\":false,\"backend\":\"cv610\"}}");
	} else {
		cv610_audio_get_stats(ctx->audio, &frames, &bytes, &packets,
			&drops);
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"enabled\":true,\"backend\":\"cv610\","
			"\"running\":%s,\"codec\":\"opus\","
			"\"sample_rate\":48000,\"channels\":1,"
			"\"muted\":%s,"
			"\"frames\":%llu,\"bytes\":%llu,"
			"\"packets\":%llu,\"drops\":%llu}}",
			cv610_audio_is_running(ctx->audio) ? "true" : "false",
			ctx->config.audio.mute ? "true" : "false",
			(unsigned long long)frames, (unsigned long long)bytes,
			(unsigned long long)packets, (unsigned long long)drops);
	}
	if (pos < 0 || pos >= (int)sizeof(buf))
		return NULL;
	return strdup(buf);
}

static const VencApplyCallbacks g_cv610_apply_callbacks = {
	.apply_bitrate = cv610_apply_bitrate,
	.apply_gop = cv610_apply_gop,
	.apply_qp_delta = cv610_apply_qp_delta,
	.apply_verbose = cv610_apply_verbose,
	.request_idr = cv610_request_idr,
	.query_live_fps = cv610_query_live_fps,
	.apply_max_payload_size = cv610_apply_max_payload_size,
	.query_transport_status = cv610_query_transport_status,
	.query_audio_status = cv610_query_audio_status,
	.query_iq_info = cv610_iq_query,
	.query_awb_info = cv610_awb_query,
	.apply_iq_param = cv610_iq_set,
	.apply_qp_bounds = cv610_apply_qp_bounds,
	/* Same shared implementation Star6E and Maruko register; it dispatches
	 * to venc_jpeg_backend_set_quality() under the snapshot module lock, so
	 * a live q change cannot interleave with a capture in progress. Without
	 * it, /api/v1/capabilities advertises snapshot.quality supported while
	 * every write to it 501s. */
	.apply_snapshot_quality = venc_jpeg_set_quality,
};

static void cv610_signal_handler(int signo)
{
	(void)signo;
	cv610_pipeline_request_stop();
}

static int cv610_output_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	int ret;

	ret = output_socket_send_parts(ctx->socket_handle, &ctx->destination,
		ctx->destination_len, ctx->connected_udp, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
	if (ret != 0) {
		__atomic_add_fetch(&ctx->output_drops, 1, __ATOMIC_RELAXED);
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
			return 0;
	} else {
		__atomic_add_fetch(&ctx->packets_sent, 1, __ATOMIC_RELAXED);
	}
	return ret;
}

static int cv610_frame_is_idr(const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		uint8_t type = h26x_util_hevc_nalu_type(nal, nal_len);
		if (type == 19 || type == 20)
			return 1;
	}
	return 0;
}

static uint32_t cv610_frame_vcl_nal_count(const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;
	uint32_t count = 0;

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		if (h26x_util_hevc_nalu_type(nal, nal_len) <= 31)
			count++;
	}
	return count;
}

static int cv610_send_rtp_frame(Cv610RunnerContext *ctx,
	const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;
	int nal_count = 0;
	int status = 0;
	uint16_t max_payload;

	max_payload = __atomic_load_n(&ctx->max_payload_size, __ATOMIC_ACQUIRE);

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		uint8_t nal_type;
		int is_last;
		size_t sent;

		nal_type = h26x_util_hevc_nalu_type(nal, nal_len);
		nal_count++;
		h26x_param_sets_update(&ctx->param_sets, PT_H265, nal_type,
			nal, nal_len);
		is_last = cursor == len;
		if (nal_type == 19 || nal_type == 20)
			(void)hevc_rtp_prepend_param_sets(&ctx->param_sets, nal_type,
				&ctx->rtp, cv610_output_write, ctx,
				max_payload, &ctx->rtp_stats);
		sent = hevc_rtp_send_nal(nal, nal_len, &ctx->rtp,
			cv610_output_write, ctx, is_last,
			max_payload, &ctx->rtp_stats);
		if (sent != nal_len) {
			/* A fragmented NAL may fail after some FU packets were sent.
			 * Reserve the failed packet's sequence number so receivers see a
			 * loss gap, and never append later NALs to the incomplete AU. */
			ctx->rtp.seq++;
			status = -1;
			break;
		}
	}
	/* An output failure may drop this access unit, but time still advances. */
	ctx->rtp.timestamp += ctx->frame_ticks;
	return nal_count > 0 ? status : -1;
}

static int cv610_copy_stream(const ot_venc_stream *stream, uint8_t **out,
	size_t *out_len)
{
	size_t total = 0;
	size_t cursor = 0;
	uint8_t *frame;
	td_u32 i;

	for (i = 0; i < stream->pack_cnt; ++i) {
		if (stream->pack[i].len > stream->pack[i].offset)
			total += stream->pack[i].len - stream->pack[i].offset;
	}
	if (total == 0)
		return -1;
	frame = malloc(total);
	if (!frame)
		return -1;
	for (i = 0; i < stream->pack_cnt; ++i) {
		size_t chunk;

		if (stream->pack[i].len <= stream->pack[i].offset)
			continue;
		chunk = stream->pack[i].len - stream->pack[i].offset;
		memcpy(frame + cursor,
			stream->pack[i].addr + stream->pack[i].offset, chunk);
		cursor += chunk;
	}
	*out = frame;
	*out_len = total;
	return 0;
}

static int cv610_apply_encoder_config(Cv610RunnerContext *ctx,
	const Cv610EncoderConfig *enc)
{
	Cv610IntraRefreshStatus intra_status;
	Cv610RefPredStatus ref_status;
	ot_venc_intra_refresh intra;
	ot_venc_intra_refresh intra_rb;
	ot_venc_ref_param ref;
	ot_venc_ref_param ref_rb;
	ot_venc_slice_split split;
	ot_venc_slice_split split_rb;
	uint32_t delivered_slices = 1;
	uint32_t cycle_len = 0;
	td_s32 ret;

	if (!ctx || !enc)
		return -1;

	memset(&intra_status, 0, sizeof(intra_status));
	memset(&ref_status, 0, sizeof(ref_status));
	snprintf(intra_status.mode_name, sizeof(intra_status.mode_name), "%s",
		intra_refresh_mode_name(enc->intra.derived.mode));
	intra_status.mi_supported = 1;
	intra_status.target_ms = enc->intra.derived.target_ms;
	intra_status.total_rows = enc->intra.derived.total_rows;
	intra_status.requested_lines = ctx->config.video0.intra_refresh_lines;
	intra_status.effective_lines_per_p = enc->intra.refresh_num;
	intra_status.lines_clamped = enc->intra.derived.lines_clamped;
	intra_status.requested_qp = ctx->config.video0.intra_refresh_qp;
	intra_status.effective_qp = enc->intra.request_i_qp;
	intra_status.explicit_gop_sec = ctx->config.video0.gop_size;
	intra_status.effective_gop_sec = enc->intra.derived.gop_overridden
		? ctx->config.video0.gop_size : enc->intra.derived.gop_sec;
	intra_status.gop_auto = enc->intra.derived.gop_overridden
		? 0 : enc->intra.derived.gop_sec > 0.0;

	ref_status.mi_supported = 1;
	ref_status.base = enc->ref.base;
	ref_status.enhance = enc->ref.enhance;
	ref_status.pred = enc->ref.pred ? 1 : 0;

	ctx->gdr_active = 0;
	ctx->gdr_cycle_len = 0;
	ctx->gdr_counter = 0;
	ctx->svct_active = 0;
	ctx->delivered_slice_count = 1;
	ctx->slice_census_done = 0;
	ctx->requested_slice_count = enc->slice.requested_count;
	ctx->intra_refresh_active = enc->intra.enabled ? 1u : 0u;
	ctx->ref_census_frames = 0;
	memset(ctx->ref_type_counts, 0, sizeof(ctx->ref_type_counts));
	ctx->trail_n_patched = 0;

	/* Fresh channels default intra refresh off, but always write the enable
	 * bit so restart behavior does not depend on a future firmware default.
	 * Preserve the driver's inactive refresh/QP values when disabling because
	 * zero is outside some vendor revisions' accepted range. */
	memset(&intra, 0, sizeof(intra));
	ret = ss_mpi_venc_get_intra_refresh(CV610_VENC_CHN, &intra);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_get_intra_refresh=0x%x\n",
			(unsigned)ret);
		goto fail;
	}
	intra.enable = enc->intra.enabled ? TD_TRUE : TD_FALSE;
	/* COLUMN, not ROW. Under ROW the intra band is a contiguous CTB range, and
	 * on the GDR recovery frame the encoder carves it off as its own I slice
	 * and then ignores the configured row split — measured on .181 as 2 slices
	 * (addresses [0,45]) instead of 17, on the largest frame in the stream and
	 * the one carrying VPS/SPS/PPS. A column band is not contiguous in raster
	 * order, so it cannot become a slice at all and the row split survives:
	 * 17/17 slices on every frame, refresh frame stays a P slice, and its size
	 * spike drops from 1.32x to 1.07x of the stream mean. waybeam-link's
	 * spatial repair drops any frame whose slice addresses are outside the
	 * learned geometry (core/src/spatial_repair.cpp:304), so under ROW the
	 * recovery frame was exactly the frame it could never salvage. */
	intra.mode = (ot_venc_intra_refresh_mode)enc->intra.refresh_dir;
	if (enc->intra.enabled) {
		intra.refresh_num = enc->intra.refresh_num;
		intra.request_i_qp = enc->intra.request_i_qp;
	}
	ret = ss_mpi_venc_set_intra_refresh(CV610_VENC_CHN, &intra);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_set_intra_refresh=0x%x "
			"enable=%u rows=%u qp=%u\n", (unsigned)ret,
			(unsigned)intra.enable, (unsigned)intra.refresh_num,
			(unsigned)intra.request_i_qp);
		goto fail;
	}
	memset(&intra_rb, 0, sizeof(intra_rb));
	ret = ss_mpi_venc_get_intra_refresh(CV610_VENC_CHN, &intra_rb);
	if (ret != TD_SUCCESS || intra_rb.enable != intra.enable ||
	    (enc->intra.enabled &&
	     (intra_rb.mode != intra.mode ||
	      intra_rb.refresh_num != intra.refresh_num ||
	      intra_rb.request_i_qp != intra.request_i_qp))) {
		fprintf(stderr, "ERROR: CV610 intra-refresh readback mismatch "
			"ret=0x%x want=%u/%u/%u/%u got=%u/%u/%u/%u\n",
			(unsigned)ret, (unsigned)intra.enable, (unsigned)intra.mode,
			(unsigned)intra.refresh_num, (unsigned)intra.request_i_qp,
			(unsigned)intra_rb.enable, (unsigned)intra_rb.mode,
			(unsigned)intra_rb.refresh_num,
			(unsigned)intra_rb.request_i_qp);
		goto fail;
	}
	intra_status.apply_ok = 1;
	intra_status.active = enc->intra.enabled ? 1 : 0;
	intra_status.effective_lines_per_p = enc->intra.enabled
		? intra_rb.refresh_num : 0;
	intra_status.effective_qp = enc->intra.enabled
		? intra_rb.request_i_qp : 0;
	if (enc->intra.enabled && intra_rb.refresh_num > 0) {
		cycle_len = (enc->intra.derived.total_rows +
			intra_rb.refresh_num - 1u) / intra_rb.refresh_num;
		ctx->gdr_active = 1;
		ctx->gdr_cycle_len = cycle_len > 255u ? 255u : (uint8_t)cycle_len;
	}

	/* The disabled state is the fresh-channel default. Only program the
	 * reference cadence when a preset requested it, avoiding an invented
	 * "off" tuple for an API whose base field must be greater than zero. */
	memset(&ref_rb, 0, sizeof(ref_rb));
	ret = ss_mpi_venc_get_ref_param(CV610_VENC_CHN, &ref_rb);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_get_ref_param=0x%x\n",
			(unsigned)ret);
		goto fail;
	}
	if (enc->ref.enabled) {
		memset(&ref, 0, sizeof(ref));
		ref.base = enc->ref.base;
		ref.enhance = enc->ref.enhance;
		ref.pred_en = enc->ref.pred ? TD_TRUE : TD_FALSE;
		ref.base_qp_delta_en = TD_FALSE;
		ref.base_qp_delta = 0;
		ret = ss_mpi_venc_set_ref_param(CV610_VENC_CHN, &ref);
		if (ret != TD_SUCCESS) {
			fprintf(stderr, "ERROR: ss_mpi_venc_set_ref_param=0x%x "
				"base=%u enhance=%u pred=%u\n", (unsigned)ret,
				(unsigned)ref.base, (unsigned)ref.enhance,
				(unsigned)ref.pred_en);
			goto fail;
		}
		memset(&ref_rb, 0, sizeof(ref_rb));
		ret = ss_mpi_venc_get_ref_param(CV610_VENC_CHN, &ref_rb);
		if (ret != TD_SUCCESS || ref_rb.base != ref.base ||
		    ref_rb.enhance != ref.enhance ||
		    ref_rb.pred_en != ref.pred_en ||
		    ref_rb.base_qp_delta_en != ref.base_qp_delta_en ||
		    ref_rb.base_qp_delta != ref.base_qp_delta) {
			fprintf(stderr, "ERROR: CV610 ref-param readback mismatch "
				"ret=0x%x want=%u/%u/%u/%u/%d "
				"got=%u/%u/%u/%u/%d\n",
				(unsigned)ret, (unsigned)ref.base,
				(unsigned)ref.enhance, (unsigned)ref.pred_en,
				(unsigned)ref.base_qp_delta_en,
				(int)ref.base_qp_delta,
				(unsigned)ref_rb.base, (unsigned)ref_rb.enhance,
				(unsigned)ref_rb.pred_en,
				(unsigned)ref_rb.base_qp_delta_en,
				(int)ref_rb.base_qp_delta);
			goto fail;
		}
		ref_status.active = 1;
		ctx->svct_active = 1;
	}
	ref_status.apply_ok = 1;

	/* split_mode=1 is the live firmware's row-split default. Early slice
	 * output remains off: this runtime publishes one whole access unit per
	 * GetStream result to both frame-SHM and RTP. */
	memset(&split, 0, sizeof(split));
	ret = ss_mpi_venc_get_slice_split(CV610_VENC_CHN, &split);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_get_slice_split=0x%x\n",
			(unsigned)ret);
		goto fail;
	}
	split.enable = enc->slice.enabled ? TD_TRUE : TD_FALSE;
	split.slice_output_en = TD_FALSE;
	if (enc->slice.enabled) {
		split.split_mode = enc->slice.split_mode;
		split.split_size = enc->slice.split_size;
	}
	ret = ss_mpi_venc_set_slice_split(CV610_VENC_CHN, &split);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_set_slice_split=0x%x "
			"enable=%u mode=%u size=%u\n", (unsigned)ret,
			(unsigned)split.enable, (unsigned)split.split_mode,
			(unsigned)split.split_size);
		goto fail;
	}
	memset(&split_rb, 0, sizeof(split_rb));
	ret = ss_mpi_venc_get_slice_split(CV610_VENC_CHN, &split_rb);
	if (ret != TD_SUCCESS || split_rb.enable != split.enable ||
	    split_rb.slice_output_en != TD_FALSE ||
	    (enc->slice.enabled &&
	     (split_rb.split_mode != split.split_mode ||
	      split_rb.split_size != split.split_size))) {
		fprintf(stderr, "ERROR: CV610 slice readback mismatch "
			"ret=0x%x want=%u/%u/%u/0 got=%u/%u/%u/%u\n",
			(unsigned)ret, (unsigned)split.enable,
			(unsigned)split.split_mode, (unsigned)split.split_size,
			(unsigned)split_rb.enable, (unsigned)split_rb.split_mode,
			(unsigned)split_rb.split_size,
			(unsigned)split_rb.slice_output_en);
		goto fail;
	}
	if (enc->slice.enabled && split_rb.split_size > 0)
		delivered_slices = (enc->slice.total_lcu_rows +
			split_rb.split_size - 1u) / split_rb.split_size;
	ctx->delivered_slice_count = delivered_slices > 255u
		? 255u : (uint8_t)delivered_slices;

	cv610_publish_encoder_status(&intra_status, &ref_status);
	fprintf(stderr, "[waybeam] CV610 resilience: intra=%s rows=%u qp=%u "
		"cycle=%u ref=%u/%u pred=%u\n", intra_status.mode_name,
		(unsigned)intra_status.effective_lines_per_p,
		(unsigned)intra_status.effective_qp, (unsigned)cycle_len,
		(unsigned)ref_status.base, (unsigned)ref_status.enhance,
		(unsigned)ref_status.pred);
	fprintf(stderr, "[waybeam] CV610 slices: requested=%u delivered=%u "
		"mode=%u lcu_rows_per_slice=%u early_output=0\n",
		(unsigned)enc->slice.requested_count, (unsigned)delivered_slices,
		(unsigned)split_rb.split_mode, (unsigned)split_rb.split_size);
	return 0;

fail:
	cv610_publish_encoder_status(&intra_status, &ref_status);
	return -1;
}

static int cv610_venc_start(Cv610RunnerContext *ctx)
{
	Cv610EncoderConfig enc;
	ot_venc_chn_attr attr;
	ot_venc_h265_vui vui;
	ot_venc_start_param start;
	ot_mpp_chn source;
	ot_mpp_chn destination;
	uint32_t gop;
	td_s32 ret;

	if (cv610_encoder_config_derive(&ctx->config, ctx->pipeline.out_width,
			ctx->pipeline.out_height,
		ctx->pipeline.fps, &enc) != 0) {
		fprintf(stderr, "ERROR: invalid CV610 encoder resilience/slice config\n");
		return -1;
	}
	gop = ctx->config.video0.gop_size <= 0.0 ? 1u :
		(uint32_t)(ctx->config.video0.gop_size * ctx->pipeline.fps + 0.5);
	if (enc.intra.enabled && !enc.intra.derived.gop_overridden &&
	    enc.intra.derived.gop_frames > 0)
		gop = enc.intra.derived.gop_frames;
	if (gop == 0)
		gop = 1;
	memset(&attr, 0, sizeof(attr));
	attr.venc_attr.type = OT_PT_H265;
	attr.venc_attr.max_pic_width = ctx->pipeline.out_width;
	attr.venc_attr.max_pic_height = ctx->pipeline.out_height;
	attr.venc_attr.buf_size =
		((ctx->pipeline.out_width * ctx->pipeline.out_height * 3 / 4) + 63) & ~63u;
	attr.venc_attr.profile = 0;
	attr.venc_attr.is_by_frame = TD_TRUE;
	attr.venc_attr.pic_width = ctx->pipeline.out_width;
	attr.venc_attr.pic_height = ctx->pipeline.out_height;
	attr.venc_attr.h265_attr.rcn_ref_share_buf_en = TD_TRUE;
	attr.venc_attr.h265_attr.frame_buf_ratio = 75;
	attr.rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
	attr.rc_attr.h265_cbr.gop = gop;
	attr.rc_attr.h265_cbr.stats_time = 1;
	attr.rc_attr.h265_cbr.src_frame_rate = ctx->pipeline.fps;
	attr.rc_attr.h265_cbr.dst_frame_rate = ctx->pipeline.fps;
	attr.rc_attr.h265_cbr.bit_rate = ctx->config.video0.bitrate;
	attr.gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
	attr.gop_attr.normal_p.ip_qp_delta = ctx->config.video0.qp_delta;

	ret = ss_mpi_venc_create_chn(CV610_VENC_CHN, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_create_chn=0x%x\n", ret);
		return -1;
	}
	ctx->venc_created = 1;
	/* Seed from the API's own seconds->frames arithmetic — deliberately NOT the
	 * local `gop` above, and not a driver readback. This value exists only so
	 * cv610_apply_gop() can recognise the API replaying the value it believes is
	 * already in force (its rollback step) and accept it as a no-op, so it has
	 * to be computed the way the API computes it. `gop` above can diverge: it
	 * takes the intra-derived auto-GOP whenever a config enables intra refresh
	 * without pinning gop_size, and then the replay would be refused and report
	 * "rollback incomplete" against a channel nothing had touched. */
	ctx->applied_gop_frames = pipeline_common_gop_frames(
		ctx->config.video0.gop_size, ctx->pipeline.fps);
	if (cv610_apply_encoder_config(ctx, &enc) != 0)
		return -1;
	/* video0.minQp/maxQp are MUT_LIVE, but they also have to take effect on
	 * a cold boot -- the config is read before the channel exists, so the
	 * live path never runs for a value that was already in the file. */
	if (ctx->config.video0.min_qp || ctx->config.video0.max_qp) {
		/* Not (void): a rejected cold-boot apply would otherwise leave the
		 * operator booting with no QP bound and no indication. */
		if (cv610_apply_qp_bounds(ctx->config.video0.min_qp,
				ctx->config.video0.max_qp) != 0)
			fprintf(stderr, "WARN: qpBounds from config not applied "
				"(min=%u max=%u)\n",
				(unsigned)ctx->config.video0.min_qp,
				(unsigned)ctx->config.video0.max_qp);
	}
	memset(&vui, 0, sizeof(vui));
	ret = ss_mpi_venc_get_h265_vui(CV610_VENC_CHN, &vui);
	if (ret != TD_SUCCESS)
		return -1;
	vui.vui_time_info.timing_info_present_flag = 1;
	vui.vui_time_info.num_units_in_tick = 1000;
	vui.vui_time_info.time_scale = ctx->pipeline.fps * 1000;
	vui.vui_time_info.num_ticks_poc_diff_one_minus1 = 0;
	vui.vui_video_signal.video_signal_type_present_flag = 1;
	vui.vui_video_signal.video_format = 5;
	vui.vui_video_signal.video_full_range_flag = 1;
	vui.vui_video_signal.colour_description_present_flag = 1;
	vui.vui_video_signal.colour_primaries = 1;
	vui.vui_video_signal.transfer_characteristics = 1;
	vui.vui_video_signal.matrix_coefficients = 1;
	if (ss_mpi_venc_set_h265_vui(CV610_VENC_CHN, &vui) != TD_SUCCESS)
		return -1;
	memset(&start, 0, sizeof(start));
	start.recv_pic_num = -1;
	if (ss_mpi_venc_start_chn(CV610_VENC_CHN, &start) != TD_SUCCESS)
		return -1;
	ctx->venc_started = 1;
	source.mod_id = OT_ID_VPSS;
	source.dev_id = CV610_VPSS_GRP;
	source.chn_id = CV610_VPSS_CHN;
	destination.mod_id = OT_ID_VENC;
	destination.dev_id = 0;
	destination.chn_id = CV610_VENC_CHN;
	if (ss_mpi_sys_bind(&source, &destination) != TD_SUCCESS)
		return -1;
	ctx->venc_bound = 1;

	/* JPEG snapshot: a second consumer on the source the main channel just
	 * bound, registered only now so it can never join a source that failed
	 * to come up.  Non-fatal — /api/v1/snapshot.jpg serves 503 if it does
	 * not initialise.  snapshot.width/height are not honoured here (the
	 * channel shares the main stream's VPSS output), which is why they are
	 * advertised unsupported for this backend. */
	{
		const VencConfigSnapshot *snap = &ctx->config.snapshot;
		VencJpegConfig jcfg = {
			.width   = ctx->pipeline.out_width,
			.height  = ctx->pipeline.out_height,
			.quality = snap->quality,
			.channel = snap->channel,
			.enabled = snap->enabled,
		};

		venc_jpeg_set_source(&source);
		(void)venc_jpeg_init(&jcfg);
	}

	printf("> CV610 H.265 %ux%u@%u CBR=%u kbit/s GOP=%.2fs/%uf\n",
		ctx->pipeline.out_width, ctx->pipeline.out_height, ctx->pipeline.fps,
		ctx->config.video0.bitrate, ctx->config.video0.gop_size, gop);
	return 0;
}

static void cv610_venc_stop(Cv610RunnerContext *ctx)
{
	ot_mpp_chn source = { OT_ID_VPSS, CV610_VPSS_GRP, CV610_VPSS_CHN };
	ot_mpp_chn destination = { OT_ID_VENC, 0, CV610_VENC_CHN };

	/* Before the source it is bound to goes away — the snapshot channel is
	 * the other consumer of `source`. */
	venc_jpeg_shutdown();

	if (ctx->venc_bound) {
		(void)ss_mpi_sys_unbind(&source, &destination);
		ctx->venc_bound = 0;
	}
	if (ctx->venc_started) {
		(void)ss_mpi_venc_stop_chn(CV610_VENC_CHN);
		ctx->venc_started = 0;
	}
	if (ctx->venc_created) {
		(void)ss_mpi_venc_destroy_chn(CV610_VENC_CHN);
		ctx->venc_created = 0;
	}
}

static int cv610_output_start(Cv610RunnerContext *ctx)
{
	RtpSessionState session;

	if (!ctx->config.outgoing.enabled)
		return 0;
	if (ctx->output_uri.type == VENC_OUTPUT_URI_FRAME_SHM) {
		ctx->frame_ring = venc_frame_ring_create(ctx->output_uri.endpoint,
			CV610_FRAME_RING_SLOTS, CV610_FRAME_RING_BYTES);
		return ctx->frame_ring ? 0 : -1;
	}
	if (ctx->output_uri.type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "ERROR: CV610 packet-shm output is not in the first bring-up slice; use frame-shm://\n");
		return -1;
	}
	if (output_socket_configure(&ctx->socket_handle, &ctx->destination,
		&ctx->destination_len, &ctx->transport, &ctx->output_uri,
		ctx->config.outgoing.connected_udp,
		ctx->config.outgoing.allow_unix_encoder_stall,
		&ctx->connected_udp) != 0)
		return -1;
	(void)output_socket_capture_capacity(ctx->socket_handle, &ctx->send_queue);
	rtp_session_init(&session, rtp_session_payload_type(PT_H265),
		ctx->pipeline.fps);
	ctx->rtp.seq = session.seq;
	ctx->rtp.timestamp = session.timestamp;
	ctx->rtp.ssrc = session.ssrc;
	ctx->rtp.payload_type = session.payload_type;
	ctx->frame_ticks = session.frame_ticks;
	return 0;
}

static void cv610_output_stop(Cv610RunnerContext *ctx)
{
	if (ctx->frame_ring) {
		venc_frame_ring_destroy(ctx->frame_ring);
		ctx->frame_ring = NULL;
	}
	if (ctx->socket_handle >= 0) {
		close(ctx->socket_handle);
		ctx->socket_handle = -1;
	}
}

static VencConfig *cv610_config(void *opaque)
{
	return &((Cv610RunnerContext *)opaque)->config;
}

static int cv610_prepare(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	VencConfig *cfg = &ctx->config;
	const Cv610SensorMode *mode;
	int mode_index = -1;

	setvbuf(stdout, NULL, _IONBF, 0);
	/* Config loading and every staged HTTP mutation run the same lookup
	 * through cv610_validate_config(), so reaching this is a config file
	 * edited behind the daemon's back. */
	mode = cv610_mode_select(cfg->sensor.mode, cfg->video0.fps, &mode_index);
	if (mode == NULL || cv610_mode_check_output(mode, cfg->video0.width,
			cfg->video0.height) != NULL) {
		fprintf(stderr, "ERROR: CV610 cannot encode %ux%u @ %u fps\n",
			cfg->video0.width, cfg->video0.height, cfg->video0.fps);
		return 1;
	}
	/* The sensor runs at the MODE's rate.  Say so when that is not what was
	 * asked for — a forced sensor.mode and a substituted target both land
	 * here, and silence would leave every status endpoint reporting a rate
	 * the operator never chose. */
	if (mode->fps != cfg->video0.fps)
		printf("> Requested %u fps, using %u fps (sensor mode %d: %s)\n",
			cfg->video0.fps, mode->fps, mode_index, mode->desc);
	/* Publish what was actually selected, so /api/v1/modes reports the
	 * achieved mode rather than recomputing the configured one — the same
	 * contract star6e_pipeline.c fulfils after sensor_select(). */
	venc_api_set_sensor_info(0, mode_index, cfg->sensor.index);
	ctx->pipeline.width = mode->width;
	ctx->pipeline.height = mode->height;
	cv610_mode_resolve_output(mode, cfg->video0.width, cfg->video0.height,
		&ctx->pipeline.out_width, &ctx->pipeline.out_height);
	ctx->pipeline.keep_aspect = cfg->isp.keep_aspect ? 1 : 0;
	ctx->pipeline.fps = mode->fps;
	ctx->pipeline.lanes = 4;
	ctx->pipeline.data_rate_x2 = 0;
	ctx->pipeline.bayer = 0;
	ctx->pipeline.raw_bit = (int)mode->raw_bit;
	ctx->pipeline.sensor_clock_hz = mode->sensor_clock_hz;
	/* Match the standalone streamer's production graph. The CV610 module
	 * loader now provides the clean SYS/VB lifecycle required by online VI. */
	ctx->pipeline.vi_online = 1;
	ctx->pipeline.i2c_bus = 0;
	ctx->socket_handle = -1;
	ctx->live_bitrate = cfg->video0.bitrate;
	ctx->max_payload_size = cfg->outgoing.max_payload_size;
	ctx->verbose = cfg->system.verbose ? 1 : 0;
	if (cfg->outgoing.enabled &&
		venc_config_parse_output_uri(cfg->outgoing.server,
			&ctx->output_uri) != 0) {
		fprintf(stderr, "ERROR: invalid CV610 output URI: %s\n",
			cfg->outgoing.server);
		return 1;
	}
	printf("> CV610/IMX662 backend selected (initial streaming slice)\n");
	return 0;
}

static int cv610_init(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = cv610_signal_handler;
	sigemptyset(&action.sa_mask);
	(void)sigaction(SIGINT, &action, NULL);
	(void)sigaction(SIGTERM, &action, NULL);
	if (cv610_pipeline_start(&ctx->pipeline) != 0)
		return -1;
	if (cv610_venc_start(ctx) != 0)
		return -1;
	if (ctx->config.debug.show_osd) {
		ctx->debug_osd = debug_osd_create(ctx->pipeline.out_width,
			ctx->pipeline.out_height, NULL);
		if (!ctx->debug_osd)
			fprintf(stderr, "WARNING: CV610 debug OSD unavailable\n");
	}
	if (cv610_output_start(ctx) != 0)
		return -1;
	if (ctx->config.audio.enabled) {
		/* Non-fatal, as on Star6E (star6e_pipeline.c discards the audio
		 * init result): audio needs kernel modules the loader only stages
		 * when CV610_AUDIO=1, and a daemon respawn cannot load them.  A
		 * config toggle must not be able to take video down with it.
		 * /api/v1/audio/status reports the running state, not the flag. */
		ctx->audio = cv610_audio_start(&ctx->config, &ctx->output_uri);
		/* No cause is guessed here. This line used to assert "needs
		 * CV610_AUDIO=1 at module load" for every failure, which sent the
		 * operator after the module set while the real cause was a
		 * predecessor's stale AI claim. Each failure point in
		 * cv610_audio_start() names itself, including the module-absent one. */
		if (!ctx->audio)
			fprintf(stderr,
				"WARNING: CV610 audio did not start; continuing without it "
				"(cause reported above)\n");
	}
	star6e_recorder_init(&ctx->recorder);
	pthread_mutex_init(&ctx->rec_writer_lock, NULL);
	audio_ring_init(&ctx->audio_ring);
	/* CV610 audio is fixed at 48 kHz mono Opus in cv610_audio.c; the rate
	 * and channel count come from the config only so a mismatch shows up
	 * as a bad TS rather than being silently papered over.  Zero rate ==
	 * "no audio in the mux". */
	/* Set after the mutex and star6e_recorder_init above, and immediately
	 * before ts_recorder_init below — the last of the three. */
	ctx->rec_locks_ready = 1;
	star6e_ts_recorder_init(&ctx->ts_recorder,
		ctx->config.audio.enabled ? ctx->config.audio.sample_rate : 0,
		ctx->config.audio.enabled ? (uint8_t)ctx->config.audio.channels : 0,
		TS_AUDIO_CODEC_OPUS);
	if (ctx->config.record.max_seconds > 0)
		ctx->ts_recorder.max_seconds = ctx->config.record.max_seconds;
	if (ctx->config.record.max_mb > 0)
		ctx->ts_recorder.max_bytes =
			(uint64_t)ctx->config.record.max_mb * 1024 * 1024;

	g_cv610_runner = ctx;
	if (venc_api_register(&ctx->config, "cv610",
		&g_cv610_apply_callbacks, NULL) != 0)
		return -1;
	venc_api_set_record_status_fn(cv610_record_status_callback);
	venc_api_set_record_http_control_supported(true);

	/* Auto-start.  Only "mirror" is implemented here: dual and dual-stream
	 * need a second VENC channel, deliberately deferred (docs/CV610_BACKEND.md).
	 * An unimplemented mode is refused loudly rather than silently recording
	 * the wrong channel. */
	if (ctx->config.record.enabled && ctx->config.record.dir[0]) {
		if (strcmp(ctx->config.record.mode, "mirror") == 0)
			cv610_record_start(ctx, ctx->config.record.dir);
		else if (strcmp(ctx->config.record.mode, "off") != 0)
			fprintf(stderr, "WARNING: record.mode=%s is not implemented "
				"on CV610; recording not started (mirror only)\n",
				ctx->config.record.mode);
	}
	if (venc_httpd_start(ctx->config.system.web_port) != 0)
		return -1;
	return 0;
}

static void cv610_report_frame_status(Cv610RunnerContext *ctx)
{
	uint64_t audio_frames = 0;
	uint64_t audio_bytes = 0;
	uint64_t audio_packets = 0;
	uint64_t audio_drops = 0;
	uint64_t frames;

	frames = __atomic_load_n(&ctx->frames, __ATOMIC_RELAXED);
	if (frames != 1 && frames % ctx->pipeline.fps != 0)
		return;
	cv610_audio_get_stats(ctx->audio, &audio_frames, &audio_bytes,
		&audio_packets, &audio_drops);
	if (ctx->debug_osd) {
		debug_osd_begin_frame(ctx->debug_osd);
		debug_osd_text(ctx->debug_osd, 0, "fps", "%u", ctx->pipeline.fps);
		debug_osd_text(ctx->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ctx->debug_osd));
		debug_osd_text(ctx->debug_osd, 2, "enc", "%ux%u h265",
			ctx->pipeline.out_width, ctx->pipeline.out_height);
		debug_osd_text(ctx->debug_osd, 3, "br", "%uk",
			__atomic_load_n(&ctx->live_bitrate, __ATOMIC_ACQUIRE));
		debug_osd_text(ctx->debug_osd, 4, "drop", "%llu",
			(unsigned long long)__atomic_load_n(&ctx->output_drops,
				__ATOMIC_RELAXED));
		debug_osd_end_frame(ctx->debug_osd);
	}
	if (!__atomic_load_n(&ctx->verbose, __ATOMIC_ACQUIRE))
		return;
	printf("> CV610 frames=%llu bytes=%llu output_drops=%llu\n",
		(unsigned long long)frames,
		(unsigned long long)__atomic_load_n(&ctx->bytes, __ATOMIC_RELAXED),
		(unsigned long long)__atomic_load_n(&ctx->output_drops,
			__ATOMIC_RELAXED));
	if (ctx->audio)
		printf("> CV610 audio frames=%llu bytes=%llu packets=%llu drops=%llu\n",
			(unsigned long long)audio_frames,
			(unsigned long long)audio_bytes,
			(unsigned long long)audio_packets,
			(unsigned long long)audio_drops);
}

static int cv610_run(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	int venc_fd = ss_mpi_venc_get_fd(CV610_VENC_CHN);

	if (venc_fd < 0)
		return -1;
	while (!cv610_pipeline_stop_requested()) {
		ot_venc_chn_status status;
		ot_venc_stream stream;
		fd_set readfds;
		struct timeval timeout = { 1, 0 };
		uint8_t *frame = NULL;
		size_t frame_len = 0;
		td_s32 ret;
		int ready;

		if (venc_api_get_reinit()) {
			venc_api_clear_reinit();
			venc_respawn_request();
			printf("> CV610 reinit requested: cold restart via fork+exec\n");
			break;
		}

		/* HTTP-driven record control.  Read both flags before acting so a
		 * start racing a stop cannot leave the request latched. */
		{
			char rec_dir[VENC_CONFIG_STRING_MAX];
			int start_pending = venc_api_get_record_start(rec_dir,
				sizeof(rec_dir));
			int stop_pending = venc_api_get_record_stop();

			if (stop_pending && !start_pending)
				cv610_record_stop(ctx, 1);
			if (start_pending) {
				if (!rec_dir[0])
					venc_api_get_record_dir(rec_dir,
						sizeof(rec_dir));
				if (strcmp(ctx->config.record.mode, "mirror") == 0 ||
				    ctx->config.record.mode[0] == '\0')
					cv610_record_start(ctx, rec_dir);
				else
					fprintf(stderr, "WARNING: record.mode=%s is not "
						"implemented on CV610; start ignored "
						"(mirror only)\n",
						ctx->config.record.mode);
			}
		}

		FD_ZERO(&readfds);
		FD_SET(venc_fd, &readfds);
		ready = select(venc_fd + 1, &readfds, NULL, NULL, &timeout);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return -1;
		if (ready == 0)
			continue;
		memset(&status, 0, sizeof(status));
		ret = ss_mpi_venc_query_status(CV610_VENC_CHN, &status);
		if (ret != TD_SUCCESS || status.cur_packs == 0)
			continue;
		memset(&stream, 0, sizeof(stream));
		stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
		if (!stream.pack)
			return -1;
		stream.pack_cnt = status.cur_packs;
		ret = ss_mpi_venc_get_stream(CV610_VENC_CHN, &stream, 1000);
		if (ret != TD_SUCCESS) {
			free(stream.pack);
			continue;
		}
		if (cv610_copy_stream(&stream, &frame, &frame_len) == 0) {
			int is_enhance = ctx->svct_active &&
				stream.h265_info.ref_type ==
					OT_VENC_ENHANCE_P_SLICE_NOT_FOR_REF;
			int is_idr;
			size_t patched = 0;

			if (is_enhance)
				patched = h26x_util_hevc_patch_trail_r_to_n(frame, frame_len);
			if (ctx->svct_active && ctx->ref_census_frames < 64u) {
				unsigned int ref_type = (unsigned int)stream.h265_info.ref_type;

				if (ref_type < OT_VENC_P_SLICE_BUTT)
					ctx->ref_type_counts[ref_type]++;
				ctx->trail_n_patched += (uint32_t)patched;
				ctx->ref_census_frames++;
				if (ctx->ref_census_frames == 64u) {
					fprintf(stderr, "[waybeam] CV610 ref census: frames=64 "
						"types=%u,%u,%u,%u,%u,%u trail_n_patched=%u\n",
						(unsigned)ctx->ref_type_counts[0],
						(unsigned)ctx->ref_type_counts[1],
						(unsigned)ctx->ref_type_counts[2],
						(unsigned)ctx->ref_type_counts[3],
						(unsigned)ctx->ref_type_counts[4],
						(unsigned)ctx->ref_type_counts[5],
						(unsigned)ctx->trail_n_patched);
				}
			}
			is_idr = cv610_frame_is_idr(frame, frame_len);
			if (!ctx->slice_census_done &&
			    ctx->requested_slice_count > 1) {
				uint32_t vcl_nals = cv610_frame_vcl_nal_count(frame,
					frame_len);

				if (vcl_nals > 0) {
					fprintf(stderr, "[waybeam] CV610 slice census: "
						"requested=%u expected=%u actual_vcl_nals=%u\n",
						(unsigned)ctx->requested_slice_count,
						(unsigned)ctx->delivered_slice_count,
						(unsigned)vcl_nals);
					ctx->delivered_slice_count = vcl_nals > 255u
						? 255u : (uint8_t)vcl_nals;
					ctx->slice_census_done = 1;
				}
			}

			if (ctx->frame_ring) {
				VencFrameMeta meta;
				int write_ret;

				memset(&meta, 0, sizeof(meta));
				meta.pts = stream.pack_cnt ? (uint32_t)stream.pack[0].pts : 0;
				meta.codec = VENC_FRAME_CODEC_H265;
				meta.flags = is_idr ? VENC_FRAME_FLAG_IDR : 0;
				if (is_enhance)
					meta.flags |= VENC_FRAME_FLAG_ENHANCE;
				if (is_idr) {
					ctx->gdr_counter = 0;
				} else if (ctx->gdr_active && ctx->gdr_cycle_len > 0) {
					meta.flags |= VENC_FRAME_FLAG_GDR;
					meta.gdr_pos = ctx->gdr_counter;
					meta.gdr_len = ctx->gdr_cycle_len;
					ctx->gdr_counter++;
					if (ctx->gdr_counter >= ctx->gdr_cycle_len)
						ctx->gdr_counter = 0;
				}
				write_ret = venc_frame_ring_write(ctx->frame_ring, &meta,
					frame, (uint32_t)frame_len);
				if (write_ret != 0)
					__atomic_add_fetch(&ctx->output_drops, 1,
						__ATOMIC_RELAXED);
				cv610_service_ring_low_water(ctx);
			} else if (ctx->socket_handle >= 0) {
				/* cv610_output_write owns per-datagram drop accounting. */
				(void)cv610_send_rtp_frame(ctx, frame, frame_len);
			}
			__atomic_add_fetch(&ctx->frames, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&ctx->bytes, frame_len, __ATOMIC_RELAXED);
			if (ctx->debug_osd)
				debug_osd_sample_cpu(ctx->debug_osd);
			cv610_report_frame_status(ctx);

			/* Mirror-mode recording: hand the SAME buffer the
			 * transport just used to the writer thread and let it
			 * block on the disk instead of this loop.  Ownership
			 * transfers, so there is no extra copy — cv610_copy_stream
			 * already produced the contiguous access unit, and push()
			 * frees it whether it is queued or dropped.
			 *
			 * This has to stay off the drain loop: a blocking write
			 * here delays ss_mpi_venc_release_stream() below, which
			 * backs up the encoder's output queue and stalls the LIVE
			 * transport, not just the recording. */
			/* A recorder that stopped ITSELF (disk full, write
			 * error) is torn down AFTER the release below, not
			 * here — see that site for why. */
			if (ctx->rec_writer) {
				struct timespec rec_now;

				clock_gettime(CLOCK_MONOTONIC, &rec_now);
				(void)venc_rec_writer_push(ctx->rec_writer,
					frame, frame_len,
					ts_mux_timespec_to_pts(
						(uint32_t)rec_now.tv_sec,
						(uint32_t)rec_now.tv_nsec),
					is_idr);
				frame = NULL;  /* the writer owns it now */
			}
			free(frame);
		}
		ret = ss_mpi_venc_release_stream(CV610_VENC_CHN, &stream);
		free(stream.pack);

		/* A recorder that stopped ITSELF (disk full, write error) does
		 * so on the writer thread, so nothing but this loop is
		 * positioned to notice, and the gate above would go on
		 * queueing into a dead writer indefinitely.
		 *
		 * AFTER the release: this ends in a join over the access unit
		 * still in the sink's hands, which on the failing medium that
		 * triggered the stop is a blocking write.  Before the release
		 * it would hold the encoder's output slot and stall the LIVE
		 * transport — the coupling this writer exists to remove. */
		if (ctx->rec_writer &&
		    !star6e_record_wants_frame(&ctx->ts_recorder,
				&ctx->recorder))
			cv610_record_stop(ctx, 1);

		/* Rotation asked for a keyframe.  Serviced here rather than
		 * inside the recorder: after the release, coalesced (a periodic
		 * rotation is not a bootstrap event), and on this backend the
		 * flag is raised on the WRITER thread, so it is deliberately an
		 * atomic hand-off rather than a direct SDK call from there. */
		if (star6e_ts_recorder_take_idr_request(&ctx->ts_recorder))
			(void)cv610_request_idr();
		if (ret != TD_SUCCESS)
			return -1;
	}
	return 0;
}

static void cv610_teardown(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;

	venc_httpd_pause();
	venc_httpd_stop();
	g_cv610_runner = NULL;
	/* Before cv610_audio_stop(): the tee points into ctx->audio_ring, and
	 * the recorder is the only reader of it. */
	cv610_record_stop(ctx, 0);   /* full flush: shutting down anyway */
	cv610_audio_stop(ctx->audio);
	ctx->audio = NULL;
	/* AFTER cv610_audio_stop(), which is what joins the capture thread.
	 * Clearing the tee above is an atomic store and does not synchronise
	 * with a thread that already loaded a non-NULL ring, so destroying the
	 * mutex first can leave that thread locking freed primitives — and a
	 * hang there never reaches the VENC/VPSS teardown below, leaking
	 * kernel-state channels and binds that make the next start fail with
	 * EXIST.  star6e_runtime.c already orders it this way. */
	audio_ring_destroy(&ctx->audio_ring);
	if (ctx->rec_locks_ready) {
		pthread_mutex_destroy(&ctx->rec_writer_lock);
		ctx->rec_locks_ready = 0;
	}
	cv610_output_stop(ctx);
	debug_osd_destroy(ctx->debug_osd);
	ctx->debug_osd = NULL;
	cv610_venc_stop(ctx);
	cv610_pipeline_stop();
}

static int cv610_map_result(int result)
{
	return result == 0 ? 0 : 2;
}

static const BackendOps g_cv610_ops = {
	.name = "cv610",
	.context_size = sizeof(Cv610RunnerContext),
	.config = cv610_config,
	.prepare = cv610_prepare,
	.init = cv610_init,
	.run = cv610_run,
	.teardown = cv610_teardown,
	.map_pipeline_result = cv610_map_result,
};

const BackendOps *cv610_runtime_backend_ops(void)
{
	return &g_cv610_ops;
}
