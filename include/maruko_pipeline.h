#ifndef MARUKO_PIPELINE_H
#define MARUKO_PIPELINE_H

#include "imu_bmi270.h"
#include "maruko_audio.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "scene_detector.h"
#include "sensor_select.h"
#include "star6e_ts_recorder.h"

#include <signal.h>

struct DebugOsdState; /* forward declaration — see debug_osd.h */
struct MarukoDualVenc; /* forward declaration — see maruko_pipeline.c */

typedef struct {
	int active;
	uint32_t level_x100;
	uint32_t output_w;
	uint32_t output_h;
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t crop_w;
	uint32_t crop_h;
} MarukoZoomStatus;

typedef struct {
  int system_initialized;
  int sensor_enabled;
  int vif_started;
  int vpe_started;
  int venc_dev_created;
  int venc_started;
  int bound_vif_vpe;
  int bound_isp_vpe;
  int bound_vpe_venc;
  int stream_started;
  MarukoOutput output;
  volatile sig_atomic_t output_enabled;
  volatile uint32_t stored_fps;
  MI_VENC_DEV venc_device;
  MI_VENC_CHN venc_channel;
  MI_SYS_ChnPort_t vif_port;
  MI_SYS_ChnPort_t isp_port;
  MI_SYS_ChnPort_t vpe_port;
  MI_SYS_ChnPort_t venc_port;
  SensorSelectResult sensor;
  MarukoBackendConfig cfg;
  SceneDetector scene;
  struct DebugOsdState *debug_osd;  /* NULL if debug OSD disabled */
  ImuState *imu;                    /* NULL if IMU disabled or init failed */
  /* Dual VENC (gemini-style) — heap-allocated, NULL when inactive. */
  struct MarukoDualVenc *dual;
  /* TS recorder state.  Active when ts_recorder.fd >= 0; that is the
   * single source of truth (see star6e_ts_recorder_is_active()).  In
   * mirror mode the chn 0 frame loop drives it; in dual mode the chn 1
   * drain thread does. */
  Star6eTsRecorderState ts_recorder;
  /* SCL crop base after optional binning and AR-matched precrop.  Stored
   * here so maruko_pipeline_apply_zoom can reposition the zoom rect on
   * live x/y pan without recomputing pipeline geometry. */
  uint32_t scl_crop_x;
  uint32_t scl_crop_y;
  uint32_t scl_crop_w;
  uint32_t scl_crop_h;
  /* Audio capture + RTP/UDP output (Phase 5).  Inactive when
   * audio.enabled=false or libmi_ai.so is missing. */
  MarukoAudioState audio;
  /* Bridge from audio encode thread to TS recorder.  Owned here, not
   * inside the audio state, so init/teardown can be sequenced
   * independently of audio init failures. */
  AudioRing audio_recorder_ring;
} MarukoBackendContext;

/** Initialize Maruko pipeline state and load SDK libraries. */
int maruko_pipeline_init(MarukoBackendContext *ctx);

/** Configure and bind the hardware graph (sensor/ISP/VPE/VENC). */
int maruko_pipeline_configure_graph(MarukoBackendContext *ctx);

/** Run the encoding loop (blocks until shutdown signal).
 *  Returns 0 for clean exit, 1 for reinit requested, -1 for error. */
int maruko_pipeline_run(MarukoBackendContext *ctx);

/** Tear down the pipeline graph only (keep httpd and MI_SYS alive). */
void maruko_pipeline_teardown_graph(MarukoBackendContext *ctx);

/** Full teardown: pipeline + httpd + MI_SYS_Exit. */
void maruko_pipeline_teardown(MarukoBackendContext *ctx);

/** Install SIGINT/SIGTERM/SIGHUP handlers for graceful shutdown/reinit. */
void maruko_pipeline_install_signal_handlers(void);

/** Create the secondary VENC channel (chn 1) and start a thread that
 *  drains its frames.  The drain destination depends on `mode`:
 *
 *    - "dual-stream": send each frame as RTP to `server`.
 *    - "dual"       : feed each frame to ctx->ts_recorder (TS file
 *                     opened by maruko_pipeline_start_recording()).
 *
 *  Must be called AFTER bind_maruko_pipeline() has finished setting up
 *  channel 0 (the SDK probe in Phase 7 confirmed CreateChn(dev,1,...)
 *  is rejected before chn 0 is fully bound).
 *
 *  Returns 0 on success (ctx->dual is allocated and active).  On
 *  failure ctx->dual is NULL and the caller continues with chn 0
 *  only — non-fatal degradation. */
int maruko_pipeline_start_dual(MarukoBackendContext *ctx,
  uint32_t bitrate, uint32_t fps, double gop_sec,
  const char *mode, const char *server, int frame_lost);

/** Tear down the secondary VENC channel if active.  Safe to call when
 *  ctx->dual is NULL (no-op). */
void maruko_pipeline_stop_dual(MarukoBackendContext *ctx);

/** Apply SCL digital zoom pan on chn 0.  pct=0 disables (full frame).
 *  zoom_pct changes are restart-required; live calls only reposition the
 *  crop at the active output dim.  Affects ch1 mirror as well because SCL
 *  fan-out happens downstream of the shared crop.  Returns 0 on success,
 *  -1 if the graph is not started or the rect was rejected. */
int maruko_pipeline_apply_zoom(MarukoBackendContext *ctx,
  double pct, double x, double y);
void maruko_pipeline_zoom_status(MarukoZoomStatus *out);

extern volatile sig_atomic_t g_maruko_running;

/** Snapshot of the IntraRefresh configuration applied to ch0 at the most
 *  recent pipeline start.  Mirrors Star6eIntraRefreshStatus exactly so the
 *  /api/v1/intra/status handler can serialize either backend uniformly. */
typedef struct {
	char mode_name[16];             /* "off" | "fast" | "balanced" | "robust" */
	int active;
	int mi_supported;
	int apply_ok;
	uint32_t target_ms;
	uint32_t total_rows;
	uint32_t requested_lines;
	uint32_t effective_lines_per_p;
	int      lines_clamped;
	uint32_t requested_qp;
	uint32_t effective_qp;
	double   explicit_gop_sec;
	double   effective_gop_sec;
	int      gop_auto;
} MarukoIntraRefreshStatus;

void maruko_pipeline_intra_refresh_status(MarukoIntraRefreshStatus *out);

#endif /* MARUKO_PIPELINE_H */
