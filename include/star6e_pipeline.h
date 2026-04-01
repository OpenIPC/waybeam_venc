#ifndef STAR6E_PIPELINE_H
#define STAR6E_PIPELINE_H

#include "audio_ring.h"
#include "imu_bmi270.h"
#include "sdk_quiet.h"
#include "sensor_select.h"
#include "star6e_audio.h"
#include "star6e_output.h"
#include "star6e_recorder.h"
#include "star6e_ts_recorder.h"
#include "star6e_video.h"
#include "venc_config.h"

#include <pthread.h>
#include <signal.h>
#include <time.h>

struct EisState;      /* forward declaration — see eis.h */
struct DebugOsdState; /* forward declaration — see debug_osd.h */

typedef struct {
	SensorSelectResult sensor;
	MI_VENC_CHN venc_channel;
	MI_SYS_ChnPort_t vif_port;
	MI_SYS_ChnPort_t vpe_port;
	MI_SYS_ChnPort_t venc_port;
	int bound_vif_vpe;
	int bound_vpe_venc;
	Star6eOutput output;
	Star6eVideoState video;
	uint32_t image_width;
	uint32_t image_height;
	volatile sig_atomic_t output_enabled;
	volatile uint32_t stored_fps;
	Star6eAudioState audio;
	Star6eRecorderState recorder;
	Star6eTsRecorderState ts_recorder;
	AudioRing audio_ring;
	ImuState *imu;              /* NULL if IMU disabled */
	struct EisState *eis;       /* NULL if EIS disabled */
	MI_VENC_Pack_t *stream_packs;     /* pre-allocated for main loop */
	uint32_t stream_packs_cap;
	/* Dual VENC (gemini mode) — heap-allocated, NULL when inactive */
	struct Star6eDualVenc *dual;
	struct DebugOsdState *debug_osd;  /* NULL if debug OSD disabled */
} Star6ePipelineState;

/** Dual VENC channel state. Heap-allocated to avoid changing
 *  VencConfig struct size (which breaks SigmaStar ISP bin loading). */
typedef struct Star6eDualVenc {
	MI_VENC_CHN channel;
	MI_SYS_ChnPort_t port;
	int bound;
	Star6eOutput output;
	Star6eVideoState video;
	char mode[16];
	uint32_t bitrate;
	uint32_t fps;
	uint32_t gop;
	char server[64];
	/* Recording thread — drains ch1 frames independently of main loop */
	pthread_t rec_thread;
	volatile sig_atomic_t rec_running;
	volatile sig_atomic_t rec_started;
	Star6eTsRecorderState *ts_recorder;  /* borrowed, NULL in dual-stream */
	int is_dual_stream;
	MI_VENC_Pack_t *stream_packs;     /* pre-allocated for rec thread */
	uint32_t stream_packs_cap;
} Star6eDualVenc;

/** Create secondary VENC channel and bind to VPE output.
 *  Returns 0 on success (state->dual is allocated and active).
 *  On failure, state->dual is NULL and pipeline operates in mirror mode. */
int star6e_pipeline_start_dual(Star6ePipelineState *state,
	uint32_t bitrate, uint32_t fps, double gop_sec,
	const char *mode, const char *server, bool frame_lost);

/** Tear down secondary VENC channel if active. */
void star6e_pipeline_stop_dual(Star6ePipelineState *state);

/** Initialize and start the full encoder pipeline (sensor → VENC). */
int star6e_pipeline_start(Star6ePipelineState *state, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet);

/** Partial reinit: tear down VENC/output/audio and rebuild.
 *  Keeps sensor/VIF/VPE running to avoid MIPI PHY stall. */
int star6e_pipeline_reinit(Star6ePipelineState *state, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet);

/** Stop streaming, unbind hardware, and release pipeline resources. */
void star6e_pipeline_stop(Star6ePipelineState *state);

/** Disable VPE prescaler (cleanup during shutdown). */
void star6e_pipeline_vpe_scl_preset_shutdown(void);

/** Service custom 3A (AWB/AE) at regular intervals. */
void star6e_pipeline_cus3a_tick(SdkQuietState *sdk_quiet,
	struct timespec *ts_last);

/** Reset CUS3A handoff state (call on pipeline reinit). */
void star6e_pipeline_cus3a_reset(void);

/** Calculate max exposure time to avoid frame drops at target FPS. */
int star6e_pipeline_cap_exposure_for_fps(uint32_t fps, uint32_t user_cap_us);

#endif /* STAR6E_PIPELINE_H */
