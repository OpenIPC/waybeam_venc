#ifndef MARUKO_PIPELINE_H
#define MARUKO_PIPELINE_H

#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "sensor_select.h"

#include <signal.h>

typedef struct {
  uint32_t ema_size_fp8;
  uint32_t frame_count;
  uint32_t last_frame_size;
  uint8_t  last_frame_type;
  uint16_t cooldown;
  uint16_t settle_count;
  uint8_t  spike_pending;
  uint8_t  idr_inserted;
  uint8_t  complexity;
  uint8_t  scene_change;
  uint16_t frames_since_idr;
  uint16_t threshold;
  uint8_t  holdoff;
  uint8_t  consecutive_spikes;
  uint8_t  idr_enabled;
  uint8_t  warmup_done;
} MarukoSceneDetector;

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
  MarukoSceneDetector scene;
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

/** Initialize scene detector from config thresholds. */
void maruko_scene_init(MarukoSceneDetector *sd, uint16_t threshold,
	uint8_t holdoff);

/** Install SIGINT/SIGTERM/SIGHUP handlers for graceful shutdown/reinit. */
void maruko_pipeline_install_signal_handlers(void);

extern volatile sig_atomic_t g_maruko_running;

#endif /* MARUKO_PIPELINE_H */
