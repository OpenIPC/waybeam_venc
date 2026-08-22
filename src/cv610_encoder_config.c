#include "cv610_encoder_config.h"

#include <string.h>

int cv610_encoder_config_derive(const VencConfig *cfg, uint32_t height,
	uint32_t fps, Cv610EncoderConfig *out)
{
	IntraRefreshMode mode;
	uint32_t rows;
	uint32_t per;

	if (!cfg || !out || height == 0 || fps == 0 ||
	    cfg->video0.slice_count < 1 ||
	    cfg->video0.slice_count > VENC_SLICE_COUNT_MAX)
		return -1;

	memset(out, 0, sizeof(*out));
	mode = intra_refresh_parse_mode(cfg->video0.intra_refresh_mode);
	intra_refresh_compute(mode, height, fps,
		cfg->video0.intra_refresh_lines,
		cfg->video0.intra_refresh_qp,
		cfg->video0.gop_size, &out->intra.derived);
	out->intra.enabled = mode != INTRA_MODE_OFF;
	out->intra.mode = 0; /* OT_VENC_INTRA_REFRESH_ROW */
	out->intra.refresh_num = out->intra.derived.lines;
	out->intra.request_i_qp = out->intra.derived.req_iqp;

	if (cfg->video0.ref_base > 0) {
		out->ref.enabled = 1;
		out->ref.base = cfg->video0.ref_base;
		out->ref.enhance = cfg->video0.ref_enhance
			? cfg->video0.ref_enhance : 1;
		out->ref.pred = cfg->video0.ref_pred ? 1u : 0u;
	}

	rows = (height + 31u) / 32u;
	out->slice.requested_count = cfg->video0.slice_count;
	out->slice.total_lcu_rows = rows;
	out->slice.split_mode = CV610_SLICE_SPLIT_MODE_LCU_ROW;
	out->slice.split_size = 1;
	out->slice.expected_count = 1;
	if (cfg->video0.slice_count > 1) {
		per = (rows + cfg->video0.slice_count - 1u) /
			cfg->video0.slice_count;
		if (per < 1)
			per = 1;
		out->slice.enabled = 1;
		out->slice.split_size = per;
		out->slice.expected_count = (rows + per - 1u) / per;
	}

	return 0;
}
