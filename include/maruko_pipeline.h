#ifndef MARUKO_PIPELINE_H
#define MARUKO_PIPELINE_H

#include "imu_bmi270.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "scene_detector.h"
#include "sensor_select.h"

#include <signal.h>

struct DebugOsdState; /* forward declaration — see debug_osd.h */
struct MarukoDualVenc; /* forward declaration — see maruko_pipeline.c */

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
 *  drains its frames onto a UDP destination.  Mirrors
 *  star6e_pipeline_start_dual() but currently only supports the
 *  "dual-stream" variant (no on-device recording — Phase 6 territory).
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

extern volatile sig_atomic_t g_maruko_running;

#endif /* MARUKO_PIPELINE_H */
