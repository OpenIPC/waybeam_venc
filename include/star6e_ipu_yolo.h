#ifndef STAR6E_IPU_YOLO_H
#define STAR6E_IPU_YOLO_H

/*
 * Star6E IPU YOLO detection — host side.
 *
 * Adds a VPE port1 tap (NV12, model input size) feeding a detector backend
 * (see detect_plugin.h) on a dedicated reader thread.  The backend owns the
 * IPU device + decode; in Phase A the "worker" backend also draws its own
 * MI_RGN overlay, so enabling detection puts boxes on the encoded stream with
 * no host-side overlay work.
 *
 * Port1 is a single physical VPE output shared with the stab framing tap, so
 * detection and framing=stab are mutually exclusive — the caller must only
 * start detection when no port1-owning framing module is active.
 */

#include "star6e_pipeline.h"   /* Star6ePipelineState */
#include "venc_config.h"       /* VencConfig */
#include "detect_plugin.h"     /* DetectBox */

#include <stdint.h>

/* Reader publishes at most this many boxes per inference (matches the
 * reader's internal box buffer). */
#define STAR6E_DETECT_SNAP_MAX 64

/* A latest-detection snapshot copied out for the encode thread (sidecar
 * DETECT trailer).  Boxes are in model/network pixel space (corner form);
 * net_w/net_h are the dims to normalise against. */
typedef struct {
	DetectBox boxes[STAR6E_DETECT_SNAP_MAX];
	int       count;        /* detections in boxes (0 = ran, empty scene) */
	uint32_t  seq;          /* monotonic publish id (dedup / freshness)   */
	uint64_t  produced_us;  /* wb_monotonic_us at inference completion     */
	uint16_t  net_w;        /* model input width  (box coord space)        */
	uint16_t  net_h;        /* model input height                          */
	uint16_t  model_id;     /* class-table selector of the model that
	                           produced these boxes — latched WITH the boxes
	                           so a live model swap flips the wire model_id
	                           in lockstep with the first new-model DETECT,
	                           never tagging old boxes with the new id      */
} Star6eDetectSnapshot;

/* Start the detector: resolve + init the configured backend, create the
 * port1 tap, and launch the reader thread.  No-op returning 0 when
 * detect.enabled is false.  On any failure the tap is torn down and the
 * pipeline continues without detection (returns 0 — detection is best-effort,
 * never fatal to the stream).  state->detect is set when active. */
int star6e_ipu_yolo_start(Star6ePipelineState *state, const VencConfig *vcfg);

/* Live-swap the detector model without respawning the pipeline: tear down and
 * re-create only the detector plugin + VPE port1 tap channel while the encoder
 * keeps running, so the video0 RTP stream is uninterrupted.  Reads the new
 * model_path / model_id / conf_thresh / nms_iou from vcfg.  MUST run on the
 * pipeline (encode) thread — the same thread that queries the snapshot — so the
 * swap is atomic w.r.t. the DETECT consumer and the failure path (which frees
 * the context) can never race a snapshot() read.
 *
 * Returns 0 when the new model is live; -1 when the swap could not be done live
 * (net geometry changed — caller must take the MUT_RESTART path — or no
 * detector was running) or the new model failed to load (detection is left
 * cleanly OFF; best-effort, never fatal to the stream). */
int star6e_ipu_yolo_reload(Star6ePipelineState *state, const VencConfig *vcfg);

/* Stop the detector: join the reader (before DisablePort — MMU-safe), deinit
 * the backend, disable port1, free the context.  Safe when inactive. */
void star6e_ipu_yolo_stop(Star6ePipelineState *state);

/* Copy the latest published detection snapshot into *out.  Returns 1 when a
 * valid snapshot exists (detection active and at least one inference done) and
 * *out is filled, 0 otherwise (inactive / no inference yet — *out untouched).
 * Safe to call on the encode thread: takes the publish lock only for the copy,
 * never across an IPU invoke. */
int star6e_ipu_yolo_snapshot(Star6ePipelineState *state,
	Star6eDetectSnapshot *out);

#endif /* STAR6E_IPU_YOLO_H */
