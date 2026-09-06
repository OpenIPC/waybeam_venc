#include "maruko_runtime.h"
#include "maruko_ipu_yolo.h"

#include "maruko_config.h"
#include "maruko_controls.h"
#include "maruko_iq.h"
#include "maruko_pipeline.h"
#include "scene_detector.h"
#include "star6e_recorder.h"
#include "star6e_ts_recorder.h"
#include "venc_api.h"
#include "venc_config.h"
#include "venc_respawn.h"
#include "venc_httpd.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	VencConfig vcfg;
	MarukoBackendContext backend;
} MarukoRunnerContext;

/* HTTP record-status callback needs access to the live recorder state.
 * Star6E parks a static pointer to its runner in star6e_runtime.c for the
 * same reason; mirror that pattern here so /api/v1/record/status reflects
 * the daemon-config-driven TS recorder on Maruko. */
static MarukoRunnerContext *g_maruko_runner_ctx;

static void maruko_record_status_callback(VencRecordStatus *out)
{
	MarukoRunnerContext *ctx = g_maruko_runner_ctx;
	Star6eTsRecorderState *ts;
	Star6eRecorderState *rec;

	memset(out, 0, sizeof(*out));
	if (!ctx)
		return;
	ts = &ctx->backend.ts_recorder;
	rec = &ctx->backend.recorder;

	{
		uint64_t dropped;
		uint32_t peak;

		/* Under the lock: the writer is freed from the encode loop at
		 * teardown and this runs on the httpd thread.  The stored
		 * values are the fallback so a finished recording still
		 * reports what it shed. */
		pthread_mutex_lock(&ctx->backend.rec_writer_lock);
		/* Inside the lock, not just the store: a 64-bit load on ARM32 is
		 * two instructions and can straddle the encode loop's update. */
		dropped = ctx->backend.rec_dropped_frames;
		peak = ctx->backend.rec_writer_peak_depth;
		if (ctx->backend.rec_writer)
			venc_rec_writer_stats(ctx->backend.rec_writer, NULL,
				&dropped, NULL, &peak);
		dropped += ctx->backend.rec_flatten_failures;
		pthread_mutex_unlock(&ctx->backend.rec_writer_lock);
		out->dropped_frames = (uint32_t)dropped;
		out->writer_peak_depth = peak;
	}

	/* is_RECORDING, not is_active: a rotation holds fd == -1 on the writer
	 * thread, and reporting that as "not recording" makes a healthy
	 * recording blink off once per segment. */
	{
		/* ONE coherent instant per recorder.  The fields below are
		 * mutated by the writer thread during writes and segment
		 * rotation: bytes_written is 64-bit on ARM32 and path is
		 * rewritten wholesale on a rotation, so reading them in place
		 * could tear outright, and reading active, counters and path at
		 * three different instants could disagree with each other. */
		Star6eRecorderSnapshot ts_snap, rec_snap;

		star6e_ts_recorder_snapshot(ts, &ts_snap);
		star6e_recorder_snapshot(rec, &rec_snap);

		if (ts_snap.active) {
			out->active = 1;
			snprintf(out->format, sizeof(out->format), "ts");
			out->bytes_written = ts_snap.bytes_written;
			out->frames_written = ts_snap.frames_written;
			out->segments = ts_snap.segments;
			out->elapsed_ms = ts_snap.elapsed_ms;
			snprintf(out->path, sizeof(out->path), "%s",
				ts_snap.path);
			snprintf(out->stop_reason, sizeof(out->stop_reason),
				"none");
		} else if (rec_snap.active) {
			out->active = 1;
			snprintf(out->format, sizeof(out->format), "hevc");
			out->bytes_written = rec_snap.bytes_written;
			out->frames_written = rec_snap.frames_written;
			out->segments = 1;  /* HEVC recorder has no rotation */
			out->elapsed_ms = rec_snap.elapsed_ms;
			snprintf(out->path, sizeof(out->path), "%s",
				rec_snap.path);
			snprintf(out->stop_reason, sizeof(out->stop_reason),
				"none");
		} else {
			/* Which recorder ran is decided by record.format, and
			 * that field is MUT_RESTART -- so within one process only
			 * ONE of these two can ever have recorded, and the other
			 * snapshot is zeroed.  Select on the format.
			 *
			 * The previous rule ("take the TS reason unless it is
			 * manual") guessed from a value that says nothing about
			 * which session ended last: a TS write_error retained
			 * from an earlier run would outrank a later, cleanly
			 * stopped hevc recording and now, with the counters
			 * following the reason, would name that older file's
			 * path and byte count too. */
			const char *reason = "manual";
			const Star6eRecorderSnapshot *last =
				(strcmp(ctx->vcfg.record.format, "hevc") == 0) ? &rec_snap : &ts_snap;
			Star6eRecorderStopReason sr = last->last_stop_reason;

			if (sr == RECORDER_STOP_DISK_FULL)
				reason = "disk_full";
			else if (sr == RECORDER_STOP_WRITE_ERROR)
				reason = "write_error";
			else if (sr == RECORDER_STOP_SIZE_LIMIT)
				reason = "size_limit";
			/* Report what the finished recording produced, from the
			 * same snapshot the reason came from.  This branch used
			 * to leave them at zero, so a recorder that stopped on
			 * its own answered {path:"", frames:0, bytes:0} -- the
			 * operator lost both the file that was cut short and how
			 * far it got, which is the whole diagnosis for any stop
			 * that was not manual.  elapsed_ms stays out: the
			 * snapshot zeroes it when inactive by contract. */
			out->bytes_written = last->bytes_written;
			out->frames_written = last->frames_written;
			out->segments = last->segments;
			snprintf(out->path, sizeof(out->path), "%s", last->path);
			snprintf(out->stop_reason, sizeof(out->stop_reason),
				"%s", reason);
		}
	}
}

static void maruko_bind_controls(MarukoRunnerContext *ctx)
{
	maruko_controls_bind(&ctx->backend, &ctx->vcfg);
}

static void maruko_reset_scene(MarukoBackendContext *backend)
{
	scene_init(&backend->scene, backend->cfg.scene_threshold,
		backend->cfg.scene_holdoff);
}

static int maruko_runner_init(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;
	MarukoBackendContext *backend = &ctx->backend;
	int ret;

	venc_httpd_start(ctx->vcfg.system.web_port);

	ret = maruko_pipeline_init(backend);
	if (ret != 0)
		return ret;

	ret = maruko_pipeline_configure_graph(backend);
	if (ret != 0)
		return ret;

	/* Bring up the framing module (video0.framing="stab"/"stab-fill") now
	 * that the SCL base crop is set and VENC is up.  No-op when framing is
	 * off; non-fill module failures degrade gracefully (RING leg intact).
	 * A stab-fill failure is FATAL: the fill module is the frame-base VENC's
	 * only producer, so continuing would stream nothing forever. */
	ret = maruko_pipeline_framing_setup(backend, &ctx->vcfg);
	if (ret != 0) {
		fprintf(stderr, "ERROR: [maruko] stab-fill framing bring-up "
			"failed — aborting init (no VENC producer)\n");
		return ret;
	}
	/*
	 * Detection owns SCL port 3.  Framing currently moves only port 0's
	 * crop, so suppress detection when stabilization is active rather than
	 * publishing boxes in the wrong encoded-frame coordinate space.
	 */
	if (ctx->vcfg.detect.enabled &&
	    (strcmp(ctx->vcfg.video0.framing, "off") != 0 ||
	     ctx->vcfg.video0.zoom_pct > 0.0)) {
		fprintf(stderr, "[maruko-ipu] detection disabled while framing=%s "
			"zoom=%.2f (coordinate mapping is not yet shared)\n",
			ctx->vcfg.video0.framing, ctx->vcfg.video0.zoom_pct);
	} else {
		(void)maruko_ipu_yolo_start(backend, &ctx->vcfg);
	}

	maruko_iq_init();
	maruko_bind_controls(ctx);
	maruko_reset_scene(backend);
	venc_api_register(&ctx->vcfg, "maruko", maruko_controls_callbacks(), NULL);
	g_maruko_runner_ctx = ctx;
	/* Explicit, not positional: the callback below takes rec_writer_lock on
	 * the httpd thread, which is already accepting.  Maruko happened to get
	 * this right via configure_graph(); saying so here means a later
	 * reorder cannot silently undo it. */
	mk_mirror_record_locks_init_public(&ctx->backend);
	venc_api_set_record_status_fn(maruko_record_status_callback);
	venc_api_set_record_http_control_supported(true);
	/* fpv.roi* are MUT_LIVE and were only ever applied by the live path, so
	 * a craft that carried roiQp in its config booted with every ROI region
	 * unprogrammed while /api/v1/capabilities advertised the field
	 * supported.  Measured on the Maruko bench: boot logged the qpDelta
	 * apply below and no ROI line at all with roiEnabled:true, roiQp:-8 in
	 * the file; one live set then logged the band immediately.  Star6E has
	 * always applied it here (star6e_runtime_apply_startup_controls) -- this
	 * is the missing half, not a new control. */
	if (ctx->vcfg.fpv.roi_enabled &&
	    maruko_controls_callbacks()->apply_roi_qp) {
		/* Not (void): maruko_apply_roi_qp() returns -1 with NO output at
		 * all when the frame geometry is still zero, which would
		 * reproduce the exact symptom this call site exists to fix -- a
		 * boot with roiQp configured and no ROI line in the log.  Same
		 * reporting as the qpBounds apply below and the CV610 twin. */
		if (maruko_controls_callbacks()->apply_roi_qp(
			    ctx->vcfg.fpv.roi_qp) != 0)
			fprintf(stderr, "WARN: ROI from config not applied "
				"(qp=%+d steps=%u center=%.2f)\n",
				ctx->vcfg.fpv.roi_qp,
				(unsigned)ctx->vcfg.fpv.roi_steps,
				ctx->vcfg.fpv.roi_center);
	}
	if (ctx->vcfg.video0.qp_delta != 0 &&
	    maruko_controls_callbacks()->apply_qp_delta) {
		maruko_controls_callbacks()->apply_qp_delta(
			ctx->vcfg.video0.qp_delta);
	}
	/* video0.minQp/maxQp are MUT_LIVE but must also take effect on a cold
	 * boot: the config is read before the channel exists, so the live path
	 * never runs for a value already in the file. */
	if (ctx->vcfg.video0.min_qp > 0 || ctx->vcfg.video0.max_qp > 0) {
		const VencApplyCallbacks *cb = maruko_controls_callbacks();
		/* Not (void): apply_qp_bounds() refuses on a VBR/AVBR rcMode,
		 * and a rejected cold-boot apply would otherwise leave the
		 * operator booting with no QP bound and no indication.  Same
		 * reporting as the CV610 cold-boot apply. */
		if (cb->apply_qp_bounds &&
		    cb->apply_qp_bounds(ctx->vcfg.video0.min_qp,
			    ctx->vcfg.video0.max_qp) != 0)
			fprintf(stderr, "WARN: qpBounds from config not applied "
				"(min=%u max=%u)\n",
				(unsigned)ctx->vcfg.video0.min_qp,
				(unsigned)ctx->vcfg.video0.max_qp);
	}

	return 0;
}

static int maruko_runner_run(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;
	int result = maruko_pipeline_run(&ctx->backend);

	if (result != 1)
		return result;

	/* Pause HTTP dispatch for the entire teardown + respawn
	 * window.  pause() drains any handler already in flight
	 * before returning; subsequent requests get 503 until the
	 * fresh respawn child accepts again. */
	venc_httpd_pause();

	/* Maruko always respawns on reinit.  Empirical evidence
	 * (2026-05-15 bench, S1 sweep): in-process reinit can
	 * page-fault inside MI_SYS_IMPL_FlushInputPortTasks during
	 * teardown of ANY MUT_RESTART transition — observed on
	 * patrol→quality (ref_*=0 throughout, only intra mode
	 * changed).  The page fault zombies the process and
	 * requires a physical reboot to clear.
	 *
	 * Trading ~2 s of additional latency (fork+exec respawn vs
	 * in-process reconfigure) for elimination of the zombie
	 * regime is unambiguously the right call.  Star6E's
	 * star6e_runtime_handle_reinit() already does this; Maruko
	 * now matches.
	 *
	 * teardown_graph() is deliberately NOT called here — the
	 * backend->teardown callback (maruko_runner_teardown ->
	 * maruko_pipeline_teardown) will run it once, cleanly, from
	 * main() after the run loop exits.  Tearing down twice
	 * (once here, once from backend->teardown) is what
	 * zombied the process on the first attempt at this fix. */
	venc_respawn_request();
	printf("> [maruko] respawn requested, "
		"exiting run loop for fork+exec\n");
	return 0;
}

static void maruko_runner_teardown(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;

	maruko_pipeline_teardown(&ctx->backend);
}

static int maruko_prepare(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (maruko_config_from_venc(&ctx->vcfg, &ctx->backend.cfg) != 0) {
		return 1;
	}

	printf("> Maruko backend selected\n");
	g_maruko_running = 1;
	maruko_pipeline_install_signal_handlers();

	ctx->backend.output.socket_handle = -1;
	ctx->backend.venc_channel = 0;
	return 0;
}

static VencConfig *maruko_config(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;

	return &ctx->vcfg;
}

static int maruko_map_pipeline_result(int result)
{
	return result == 0 ? 0 : 2;
}

static const BackendOps g_backend_ops = {
	.name = "maruko",
	.context_size = sizeof(MarukoRunnerContext),
	.config = maruko_config,
	.prepare = maruko_prepare,
	.init = maruko_runner_init,
	.run = maruko_runner_run,
	.teardown = maruko_runner_teardown,
	.map_pipeline_result = maruko_map_pipeline_result,
};

const BackendOps *maruko_runtime_backend_ops(void)
{
	return &g_backend_ops;
}
