#include "telemetry.h"

#include <string.h>

void telemetry_init(TelemetryState *ts)
{
	if (!ts)
		return;

	memset(ts, 0, sizeof(*ts));
}

void telemetry_record(TelemetryState *ts, const EncoderFrameStats *stats,
	const SceneEstimate *scene, GopState gop_state,
	uint16_t frames_since_idr, int idr_inserted)
{
	TelemetryRecord *rec;

	if (!ts || !stats)
		return;

	rec = &ts->records[ts->write_idx % TELEMETRY_RING_SLOTS];

	rec->frame_seq = stats->frame_seq;
	rec->frame_size = stats->frame_size_bytes;
	rec->frame_type = stats->frame_type;
	rec->qp = stats->qp_avg;
	rec->complexity = scene ? scene->complexity : 0;
	rec->scene_change = scene ? scene->scene_change : 0;
	rec->gop_state = (uint8_t)gop_state;
	rec->idr_inserted = idr_inserted ? 1 : 0;
	rec->frames_since_idr = frames_since_idr;

	ts->write_idx++;
	ts->count++;
}

int telemetry_get_latest(const TelemetryState *ts, TelemetryRecord *out)
{
	uint32_t idx;

	if (!ts || !out || ts->count == 0)
		return -1;

	idx = (ts->write_idx + TELEMETRY_RING_SLOTS - 1) % TELEMETRY_RING_SLOTS;
	*out = ts->records[idx];
	return 0;
}

uint32_t telemetry_count(const TelemetryState *ts)
{
	if (!ts)
		return 0;
	return ts->count;
}
