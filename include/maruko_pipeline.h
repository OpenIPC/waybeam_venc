#ifndef MARUKO_PIPELINE_H
#define MARUKO_PIPELINE_H

#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "scene_detector.h"
#include "sensor_select.h"

#include <signal.h>

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

extern volatile sig_atomic_t g_maruko_running;

#endif /* MARUKO_PIPELINE_H */
