#ifndef ENC_TELEMETRY_H
#define ENC_TELEMETRY_H

/*
 * Telemetry: binary ring buffer (always on) + optional text debug log.
 * Binary log is overwrite-oldest, zero-allocation steady state.
 */

#include "enc_types.h"

#include <stdint.h>

/* Binary telemetry record — packed for compactness */
typedef struct {
	uint32_t frame_seq;
	uint32_t frame_size;
	uint8_t  frame_type;
	uint8_t  qp;
	uint8_t  complexity;
	uint8_t  scene_change;
	uint8_t  gop_state;
	uint8_t  idr_inserted;
	uint16_t frames_since_idr;
} TelemetryRecord;

#define TELEMETRY_RING_SLOTS 600  /* 10 sec at 60fps */

typedef struct {
	TelemetryRecord records[TELEMETRY_RING_SLOTS];
	uint32_t write_idx;
	uint32_t count;            /* total records written */
	uint8_t  text_log_enabled;
	uint8_t  _pad[3];
} TelemetryState;

/** Initialize telemetry. text_log: enable human-readable stderr output. */
void telemetry_init(TelemetryState *ts, int text_log);

/** Record one frame's telemetry. */
void telemetry_record(TelemetryState *ts, const EncoderFrameStats *stats,
	const SceneEstimate *scene, GopState gop_state,
	uint16_t frames_since_idr, int idr_inserted);

/** Dump recent telemetry to stderr (last N entries). */
void telemetry_dump(const TelemetryState *ts, uint32_t last_n);

/** Get total record count. */
uint32_t telemetry_count(const TelemetryState *ts);

#endif /* ENC_TELEMETRY_H */
