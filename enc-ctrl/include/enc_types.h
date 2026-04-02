#ifndef ENC_TYPES_H
#define ENC_TYPES_H

#include <stdint.h>

/* ── Per-frame encoder feedback ─────────────────────────────────────── */

/* Frame type classification */
#define ENC_FRAME_P   0
#define ENC_FRAME_I   1
#define ENC_FRAME_IDR 2

typedef struct {
	uint32_t frame_seq;
	uint32_t frame_size_bytes;
	uint8_t  frame_type;        /* ENC_FRAME_P / ENC_FRAME_I / ENC_FRAME_IDR */
	uint8_t  qp_avg;            /* from SDK startQual or NAL parse; 0 if unavailable */
	uint8_t  qp_max;            /* reserved, 0 if unavailable */
	uint8_t  _pad0;
	uint32_t encode_time_us;    /* wall-clock encode latency; 0 if not measured */
	uint64_t timestamp_us;      /* capture timestamp from MI_VENC stream */
} EncoderFrameStats;

/* ── Scene complexity estimate ──────────────────────────────────────── */

typedef struct {
	uint8_t  complexity;        /* 0-255 quantized complexity level */
	uint8_t  scene_change;      /* 1 = probable scene change detected */
	uint16_t budget_ratio;      /* actual_size / target_size * 1000 */
} SceneEstimate;

/* ── Variable GOP state machine states ──────────────────────────────── */

typedef enum {
	GOP_STATE_NORMAL = 0,       /* encoding P-frames, monitoring */
	GOP_STATE_IDR_READY,        /* will insert IDR on next frame */
	GOP_STATE_IDR_DEFERRED,     /* IDR pending but externally deferred */
	GOP_STATE_FORCED_IDR,       /* max_gop_length reached, forcing IDR */
} GopState;

/* ── GOP controller configuration ───────────────────────────────────── */

typedef struct {
	uint16_t max_gop_length;        /* hard ceiling for frames between IDRs (e.g. 300) */
	uint16_t min_gop_length;        /* minimum frames between IDRs (e.g. 30) */
	uint16_t defer_timeout_frames;  /* max frames to defer a pending IDR (e.g. 60) */
	uint16_t scene_change_threshold;/* frame_size spike ratio * 100 (e.g. 250 = 2.5x) */
	uint8_t  scene_change_holdoff;  /* consecutive frames above threshold (e.g. 2) */
	uint8_t  enable_variable_gop;   /* 0 = passthrough (stats only), 1 = active GOP */
	uint8_t  idr_qp_boost;         /* QP steps to add for IDR size control (e.g. 4) */
	uint8_t  _pad0;
} GopConfig;

/* ── Default configuration values ───────────────────────────────────── */

#define GOP_CONFIG_DEFAULT_MAX_GOP          300
#define GOP_CONFIG_DEFAULT_MIN_GOP          30
#define GOP_CONFIG_DEFAULT_DEFER_TIMEOUT    60
#define GOP_CONFIG_DEFAULT_SC_THRESHOLD     325  /* 3.25x */
#define GOP_CONFIG_DEFAULT_SC_HOLDOFF       2
#define GOP_CONFIG_DEFAULT_IDR_QP_BOOST     4

static inline void gop_config_defaults(GopConfig *cfg)
{
	cfg->max_gop_length = GOP_CONFIG_DEFAULT_MAX_GOP;
	cfg->min_gop_length = GOP_CONFIG_DEFAULT_MIN_GOP;
	cfg->defer_timeout_frames = GOP_CONFIG_DEFAULT_DEFER_TIMEOUT;
	cfg->scene_change_threshold = GOP_CONFIG_DEFAULT_SC_THRESHOLD;
	cfg->scene_change_holdoff = GOP_CONFIG_DEFAULT_SC_HOLDOFF;
	cfg->enable_variable_gop = 0;
	cfg->idr_qp_boost = GOP_CONFIG_DEFAULT_IDR_QP_BOOST;
	cfg->_pad0 = 0;
}

#endif /* ENC_TYPES_H */
