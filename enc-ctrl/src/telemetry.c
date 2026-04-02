#include "telemetry.h"

#include <stdio.h>
#include <string.h>

void telemetry_init(TelemetryState *ts, int text_log)
{
	if (!ts)
		return;

	memset(ts, 0, sizeof(*ts));
	ts->text_log_enabled = text_log ? 1 : 0;
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

	if (ts->text_log_enabled) {
		const char *type_str = "P";
		if (stats->frame_type == ENC_FRAME_IDR)
			type_str = "IDR";
		else if (stats->frame_type == ENC_FRAME_I)
			type_str = "I";

		fprintf(stderr,
			"[enc_ctrl] seq=%u type=%s size=%u qp=%u cplx=%u sc=%u "
			"gop=%u fsi=%u%s\n",
			stats->frame_seq, type_str, stats->frame_size_bytes,
			stats->qp_avg, rec->complexity, rec->scene_change,
			(unsigned)gop_state, (unsigned)frames_since_idr,
			idr_inserted ? " [IDR_INSERT]" : "");
	}
}

void telemetry_dump(const TelemetryState *ts, uint32_t last_n)
{
	uint32_t start;
	uint32_t count;
	uint32_t i;

	if (!ts)
		return;

	count = ts->count < TELEMETRY_RING_SLOTS
		? ts->count : TELEMETRY_RING_SLOTS;

	if (last_n > count)
		last_n = count;

	if (last_n == 0)
		return;

	/* Start from (write_idx - last_n) in the ring */
	start = (ts->write_idx + TELEMETRY_RING_SLOTS - last_n)
		% TELEMETRY_RING_SLOTS;

	fprintf(stderr, "[enc_ctrl] telemetry dump (last %u):\n", last_n);
	fprintf(stderr, "  seq      type  size     qp  cplx sc gop fsi\n");

	for (i = 0; i < last_n; i++) {
		const TelemetryRecord *rec;
		uint32_t idx = (start + i) % TELEMETRY_RING_SLOTS;
		const char *type_str;

		rec = &ts->records[idx];

		type_str = "P";
		if (rec->frame_type == ENC_FRAME_IDR)
			type_str = "IDR";
		else if (rec->frame_type == ENC_FRAME_I)
			type_str = "I";

		fprintf(stderr, "  %-8u %-4s %-8u %-3u %-4u %u   %u   %u%s\n",
			rec->frame_seq, type_str, rec->frame_size,
			rec->qp, rec->complexity, rec->scene_change,
			rec->gop_state, rec->frames_since_idr,
			rec->idr_inserted ? " *" : "");
	}
}

uint32_t telemetry_count(const TelemetryState *ts)
{
	if (!ts)
		return 0;
	return ts->count;
}
