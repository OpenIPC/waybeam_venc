#include "cv610_runtime.h"

#include "cv610_audio.h"
#include "cv610_pipeline.h"
#include "debug_osd.h"
#include "h26x_param_sets.h"
#include "h26x_util.h"
#include "hevc_rtp.h"
#include "idr_rate_limit.h"
#include "output_socket.h"
#include "rtp_session.h"
#include "venc_frame_ring.h"
#include "venc_api.h"
#include "venc_httpd.h"
#include "venc_respawn.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "ot_common.h"
#include "ot_common_sys.h"
#include "ot_common_venc.h"
#include "ss_mpi_sys_bind.h"
#include "ss_mpi_venc.h"

#define CV610_VI_PIPE 0
#define CV610_VI_CHN 0
#define CV610_VENC_CHN 0
#define CV610_FRAME_RING_SLOTS 8
#define CV610_FRAME_RING_BYTES (512u * 1024u)

typedef struct {
	VencConfig config;
	Cv610PipelineConfig pipeline;
	VencOutputUri output_uri;
	int socket_handle;
	struct sockaddr_storage destination;
	socklen_t destination_len;
	VencOutputUriType transport;
	int connected_udp;
	OutputSocketQueue send_queue;
	venc_frame_ring_t *frame_ring;
	RtpPacketizerState rtp;
	H26xParamSets param_sets;
	DebugOsdState *debug_osd;
	Cv610AudioState *audio;
	HevcRtpStats rtp_stats;
	uint32_t frame_ticks;
	uint32_t live_bitrate;
	uint16_t max_payload_size;
	int verbose;
	uint64_t frames;
	uint64_t bytes;
	uint64_t output_drops;
	int venc_created;
	int venc_started;
	int venc_bound;
} Cv610RunnerContext;

static Cv610RunnerContext *g_cv610_runner;

static int cv610_update_venc_attr(uint32_t bitrate, uint32_t gop,
	int qp_delta, unsigned int fields)
{
	ot_venc_chn_attr attr;
	td_s32 ret;

	memset(&attr, 0, sizeof(attr));
	ret = ss_mpi_venc_get_chn_attr(CV610_VENC_CHN, &attr);
	if (ret != TD_SUCCESS || attr.rc_attr.rc_mode != OT_VENC_RC_MODE_H265_CBR)
		return -1;
	if (fields & 1u)
		attr.rc_attr.h265_cbr.bit_rate = bitrate;
	if (fields & 2u)
		attr.rc_attr.h265_cbr.gop = gop;
	if (fields & 4u)
		attr.gop_attr.normal_p.ip_qp_delta = qp_delta;
	ret = ss_mpi_venc_set_chn_attr(CV610_VENC_CHN, &attr);
	return ret == TD_SUCCESS ? 0 : -1;
}

static int cv610_apply_bitrate(uint32_t kbps)
{
	int ret = cv610_update_venc_attr(kbps, 0, 0, 1u);

	if (ret == 0 && g_cv610_runner)
		__atomic_store_n(&g_cv610_runner->live_bitrate, kbps,
			__ATOMIC_RELEASE);
	return ret;
}

static int cv610_apply_gop(uint32_t frames)
{
	return cv610_update_venc_attr(0, frames, 0, 2u);
}

static int cv610_apply_qp_delta(int delta)
{
	return cv610_update_venc_attr(0, 0, delta, 4u);
}

static int cv610_apply_verbose(bool on)
{
	if (!g_cv610_runner)
		return -1;
	__atomic_store_n(&g_cv610_runner->verbose, on ? 1 : 0,
		__ATOMIC_RELEASE);
	return 0;
}

static int cv610_apply_max_payload_size(uint16_t size)
{
	Cv610RunnerContext *ctx = g_cv610_runner;

	if (!ctx)
		return -1;
	__atomic_store_n(&ctx->max_payload_size, size, __ATOMIC_RELEASE);
	return 0;
}

static int cv610_request_idr(void)
{
	if (!idr_rate_limit_allow(CV610_VENC_CHN))
		return 0;
	return ss_mpi_venc_request_idr(CV610_VENC_CHN, TD_TRUE) == TD_SUCCESS
		? 0 : -1;
}

static uint32_t cv610_query_live_fps(void)
{
	return g_cv610_runner ? g_cv610_runner->pipeline.fps : 0;
}

static char *cv610_query_transport_status(void)
{
	Cv610RunnerContext *ctx = g_cv610_runner;
	char buf[384];
	const char *transport;
	uint8_t fill = 0;
	int fill_valid = 0;
	uint64_t frames;
	uint64_t bytes;
	uint64_t drops;

	if (!ctx)
		return NULL;
	transport = ctx->frame_ring ? "frame-shm" :
		(ctx->transport == VENC_OUTPUT_URI_UNIX ? "unix" : "udp");
	if (ctx->socket_handle >= 0 &&
		output_socket_get_fill_pct(ctx->socket_handle, &ctx->send_queue,
			&fill) == 0)
		fill_valid = 1;
	frames = __atomic_load_n(&ctx->frames, __ATOMIC_RELAXED);
	bytes = __atomic_load_n(&ctx->bytes, __ATOMIC_RELAXED);
	drops = __atomic_load_n(&ctx->output_drops, __ATOMIC_RELAXED);
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"transport\":\"%s\","
		"\"enabled\":%s,\"queue_fill_pct\":%u,\"queue_fill_valid\":%s,"
		"\"frames\":%llu,\"bytes\":%llu,\"drops\":%llu}}",
		transport, ctx->config.outgoing.enabled ? "true" : "false",
		(unsigned int)fill, fill_valid ? "true" : "false",
		(unsigned long long)frames, (unsigned long long)bytes,
		(unsigned long long)drops);
	return strdup(buf);
}

static const VencApplyCallbacks g_cv610_apply_callbacks = {
	.apply_bitrate = cv610_apply_bitrate,
	.apply_gop = cv610_apply_gop,
	.apply_qp_delta = cv610_apply_qp_delta,
	.apply_verbose = cv610_apply_verbose,
	.request_idr = cv610_request_idr,
	.query_live_fps = cv610_query_live_fps,
	.apply_max_payload_size = cv610_apply_max_payload_size,
	.query_transport_status = cv610_query_transport_status,
};

static void cv610_signal_handler(int signo)
{
	(void)signo;
	cv610_pipeline_request_stop();
}

static int cv610_fps_supported(uint32_t fps)
{
	return fps == 30 || fps == 60 || fps == 90 || fps == 100;
}

static int cv610_output_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	int ret;

	ret = output_socket_send_parts(ctx->socket_handle, &ctx->destination,
		ctx->destination_len, ctx->connected_udp, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
	if (ret != 0) {
		__atomic_add_fetch(&ctx->output_drops, 1, __ATOMIC_RELAXED);
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
			return 0;
	}
	return ret;
}

static int cv610_frame_is_idr(const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		uint8_t type = h26x_util_hevc_nalu_type(nal, nal_len);
		if (type == 19 || type == 20)
			return 1;
	}
	return 0;
}

static int cv610_send_rtp_frame(Cv610RunnerContext *ctx,
	const uint8_t *frame, size_t len)
{
	size_t cursor = 0;
	const uint8_t *nal;
	size_t nal_len;
	int nal_count = 0;
	int status = 0;
	uint16_t max_payload;

	max_payload = __atomic_load_n(&ctx->max_payload_size, __ATOMIC_ACQUIRE);

	while (h26x_util_annexb_next(frame, len, &cursor, &nal, &nal_len)) {
		uint8_t nal_type;
		int is_last;

		nal_type = h26x_util_hevc_nalu_type(nal, nal_len);
		nal_count++;
		h26x_param_sets_update(&ctx->param_sets, PT_H265, nal_type,
			nal, nal_len);
		is_last = cursor == len;
		if (nal_type == 19 || nal_type == 20)
			(void)hevc_rtp_prepend_param_sets(&ctx->param_sets, nal_type,
				&ctx->rtp, cv610_output_write, ctx,
				max_payload, &ctx->rtp_stats);
		if (hevc_rtp_send_nal(nal, nal_len, &ctx->rtp,
			cv610_output_write, ctx, is_last,
			max_payload, &ctx->rtp_stats) == 0) {
			status = -1;
			break;
		}
	}
	/* An output failure may drop this access unit, but time still advances. */
	ctx->rtp.timestamp += ctx->frame_ticks;
	return nal_count > 0 ? status : -1;
}

static int cv610_copy_stream(const ot_venc_stream *stream, uint8_t **out,
	size_t *out_len)
{
	size_t total = 0;
	size_t cursor = 0;
	uint8_t *frame;
	td_u32 i;

	for (i = 0; i < stream->pack_cnt; ++i) {
		if (stream->pack[i].len > stream->pack[i].offset)
			total += stream->pack[i].len - stream->pack[i].offset;
	}
	if (total == 0)
		return -1;
	frame = malloc(total);
	if (!frame)
		return -1;
	for (i = 0; i < stream->pack_cnt; ++i) {
		size_t chunk;

		if (stream->pack[i].len <= stream->pack[i].offset)
			continue;
		chunk = stream->pack[i].len - stream->pack[i].offset;
		memcpy(frame + cursor,
			stream->pack[i].addr + stream->pack[i].offset, chunk);
		cursor += chunk;
	}
	*out = frame;
	*out_len = total;
	return 0;
}

static int cv610_venc_start(Cv610RunnerContext *ctx)
{
	ot_venc_chn_attr attr;
	ot_venc_h265_vui vui;
	ot_venc_start_param start;
	ot_mpp_chn source;
	ot_mpp_chn destination;
	uint32_t gop;
	td_s32 ret;

	gop = ctx->config.video0.gop_size <= 0.0 ? 1u :
		(uint32_t)(ctx->config.video0.gop_size * ctx->pipeline.fps + 0.5);
	if (gop == 0)
		gop = 1;
	memset(&attr, 0, sizeof(attr));
	attr.venc_attr.type = OT_PT_H265;
	attr.venc_attr.max_pic_width = ctx->pipeline.width;
	attr.venc_attr.max_pic_height = ctx->pipeline.height;
	attr.venc_attr.buf_size = ((ctx->pipeline.width * ctx->pipeline.height * 3 / 4) + 63) & ~63u;
	attr.venc_attr.profile = 0;
	attr.venc_attr.is_by_frame = TD_TRUE;
	attr.venc_attr.pic_width = ctx->pipeline.width;
	attr.venc_attr.pic_height = ctx->pipeline.height;
	attr.venc_attr.h265_attr.rcn_ref_share_buf_en = TD_TRUE;
	attr.venc_attr.h265_attr.frame_buf_ratio = 75;
	attr.rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
	attr.rc_attr.h265_cbr.gop = gop;
	attr.rc_attr.h265_cbr.stats_time = 1;
	attr.rc_attr.h265_cbr.src_frame_rate = ctx->pipeline.fps;
	attr.rc_attr.h265_cbr.dst_frame_rate = ctx->pipeline.fps;
	attr.rc_attr.h265_cbr.bit_rate = ctx->config.video0.bitrate;
	attr.gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
	attr.gop_attr.normal_p.ip_qp_delta = ctx->config.video0.qp_delta;

	ret = ss_mpi_venc_create_chn(CV610_VENC_CHN, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_venc_create_chn=0x%x\n", ret);
		return -1;
	}
	ctx->venc_created = 1;
	memset(&vui, 0, sizeof(vui));
	ret = ss_mpi_venc_get_h265_vui(CV610_VENC_CHN, &vui);
	if (ret != TD_SUCCESS)
		return -1;
	vui.vui_time_info.timing_info_present_flag = 1;
	vui.vui_time_info.num_units_in_tick = 1000;
	vui.vui_time_info.time_scale = ctx->pipeline.fps * 1000;
	vui.vui_time_info.num_ticks_poc_diff_one_minus1 = 0;
	vui.vui_video_signal.video_signal_type_present_flag = 1;
	vui.vui_video_signal.video_format = 5;
	vui.vui_video_signal.video_full_range_flag = 1;
	vui.vui_video_signal.colour_description_present_flag = 1;
	vui.vui_video_signal.colour_primaries = 1;
	vui.vui_video_signal.transfer_characteristics = 1;
	vui.vui_video_signal.matrix_coefficients = 1;
	if (ss_mpi_venc_set_h265_vui(CV610_VENC_CHN, &vui) != TD_SUCCESS)
		return -1;
	memset(&start, 0, sizeof(start));
	start.recv_pic_num = -1;
	if (ss_mpi_venc_start_chn(CV610_VENC_CHN, &start) != TD_SUCCESS)
		return -1;
	ctx->venc_started = 1;
	source.mod_id = OT_ID_VI;
	source.dev_id = CV610_VI_PIPE;
	source.chn_id = CV610_VI_CHN;
	destination.mod_id = OT_ID_VENC;
	destination.dev_id = 0;
	destination.chn_id = CV610_VENC_CHN;
	if (ss_mpi_sys_bind(&source, &destination) != TD_SUCCESS)
		return -1;
	ctx->venc_bound = 1;
	printf("> CV610 H.265 %ux%u@%u CBR=%u kbit/s GOP=%.2fs/%uf\n",
		ctx->pipeline.width, ctx->pipeline.height, ctx->pipeline.fps,
		ctx->config.video0.bitrate, ctx->config.video0.gop_size, gop);
	return 0;
}

static void cv610_venc_stop(Cv610RunnerContext *ctx)
{
	ot_mpp_chn source = { OT_ID_VI, CV610_VI_PIPE, CV610_VI_CHN };
	ot_mpp_chn destination = { OT_ID_VENC, 0, CV610_VENC_CHN };

	if (ctx->venc_bound) {
		(void)ss_mpi_sys_unbind(&source, &destination);
		ctx->venc_bound = 0;
	}
	if (ctx->venc_started) {
		(void)ss_mpi_venc_stop_chn(CV610_VENC_CHN);
		ctx->venc_started = 0;
	}
	if (ctx->venc_created) {
		(void)ss_mpi_venc_destroy_chn(CV610_VENC_CHN);
		ctx->venc_created = 0;
	}
}

static int cv610_output_start(Cv610RunnerContext *ctx)
{
	RtpSessionState session;

	if (!ctx->config.outgoing.enabled)
		return 0;
	if (ctx->output_uri.type == VENC_OUTPUT_URI_FRAME_SHM) {
		ctx->frame_ring = venc_frame_ring_create(ctx->output_uri.endpoint,
			CV610_FRAME_RING_SLOTS, CV610_FRAME_RING_BYTES);
		return ctx->frame_ring ? 0 : -1;
	}
	if (ctx->output_uri.type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "ERROR: CV610 packet-shm output is not in the first bring-up slice; use frame-shm://\n");
		return -1;
	}
	if (output_socket_configure(&ctx->socket_handle, &ctx->destination,
		&ctx->destination_len, &ctx->transport, &ctx->output_uri,
		ctx->config.outgoing.connected_udp,
		ctx->config.outgoing.allow_unix_encoder_stall,
		&ctx->connected_udp) != 0)
		return -1;
	(void)output_socket_capture_capacity(ctx->socket_handle, &ctx->send_queue);
	rtp_session_init(&session, rtp_session_payload_type(PT_H265),
		ctx->pipeline.fps);
	ctx->rtp.seq = session.seq;
	ctx->rtp.timestamp = session.timestamp;
	ctx->rtp.ssrc = session.ssrc;
	ctx->rtp.payload_type = session.payload_type;
	ctx->frame_ticks = session.frame_ticks;
	return 0;
}

static void cv610_output_stop(Cv610RunnerContext *ctx)
{
	if (ctx->frame_ring) {
		venc_frame_ring_destroy(ctx->frame_ring);
		ctx->frame_ring = NULL;
	}
	if (ctx->socket_handle >= 0) {
		close(ctx->socket_handle);
		ctx->socket_handle = -1;
	}
}

static VencConfig *cv610_config(void *opaque)
{
	return &((Cv610RunnerContext *)opaque)->config;
}

static int cv610_prepare(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	VencConfig *cfg = &ctx->config;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (((cfg->video0.width != 0 || cfg->video0.height != 0) &&
		(cfg->video0.width != 1920 || cfg->video0.height != 1080)) ||
		!cv610_fps_supported(cfg->video0.fps)) {
		fprintf(stderr, "ERROR: CV610 bring-up supports 1920x1080 at 30/60/90/100 fps\n");
		return 1;
	}
	ctx->pipeline.width = cfg->video0.width ? cfg->video0.width : 1920;
	ctx->pipeline.height = cfg->video0.height ? cfg->video0.height : 1080;
	ctx->pipeline.fps = cfg->video0.fps;
	ctx->pipeline.lanes = 4;
	ctx->pipeline.data_rate_x2 = 0;
	ctx->pipeline.bayer = 0;
	ctx->pipeline.raw_bit = cfg->video0.fps > 60 ? 10 : 12;
	/* Match the standalone streamer's production graph. The CV610 module
	 * loader now provides the clean SYS/VB lifecycle required by online VI. */
	ctx->pipeline.vi_online = 1;
	ctx->pipeline.i2c_bus = 0;
	ctx->socket_handle = -1;
	ctx->live_bitrate = cfg->video0.bitrate;
	ctx->max_payload_size = cfg->outgoing.max_payload_size;
	ctx->verbose = cfg->system.verbose ? 1 : 0;
	if (cfg->outgoing.enabled &&
		venc_config_parse_output_uri(cfg->outgoing.server,
			&ctx->output_uri) != 0) {
		fprintf(stderr, "ERROR: invalid CV610 output URI: %s\n",
			cfg->outgoing.server);
		return 1;
	}
	printf("> CV610/IMX662 backend selected (initial streaming slice)\n");
	return 0;
}

static int cv610_init(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = cv610_signal_handler;
	sigemptyset(&action.sa_mask);
	(void)sigaction(SIGINT, &action, NULL);
	(void)sigaction(SIGTERM, &action, NULL);
	if (cv610_pipeline_start(&ctx->pipeline) != 0)
		return -1;
	if (cv610_venc_start(ctx) != 0)
		return -1;
	if (ctx->config.debug.show_osd) {
		ctx->debug_osd = debug_osd_create(ctx->pipeline.width,
			ctx->pipeline.height, NULL);
		if (!ctx->debug_osd)
			fprintf(stderr, "WARNING: CV610 debug OSD unavailable\n");
	}
	if (cv610_output_start(ctx) != 0)
		return -1;
	if (ctx->config.audio.enabled) {
		ctx->audio = cv610_audio_start(&ctx->config, &ctx->output_uri);
		if (!ctx->audio)
			return -1;
	}
	g_cv610_runner = ctx;
	if (venc_api_register(&ctx->config, "cv610",
		&g_cv610_apply_callbacks, NULL) != 0)
		return -1;
	venc_api_set_config_path(VENC_CONFIG_DEFAULT_PATH);
	if (venc_httpd_start(ctx->config.system.web_port) != 0)
		return -1;
	return 0;
}

static void cv610_report_frame_status(Cv610RunnerContext *ctx)
{
	uint64_t audio_frames = 0;
	uint64_t audio_bytes = 0;
	uint64_t audio_packets = 0;
	uint64_t audio_drops = 0;
	uint64_t frames;

	frames = __atomic_load_n(&ctx->frames, __ATOMIC_RELAXED);
	if (frames != 1 && frames % ctx->pipeline.fps != 0)
		return;
	cv610_audio_get_stats(ctx->audio, &audio_frames, &audio_bytes,
		&audio_packets, &audio_drops);
	if (ctx->debug_osd) {
		debug_osd_begin_frame(ctx->debug_osd);
		debug_osd_text(ctx->debug_osd, 0, "fps", "%u", ctx->pipeline.fps);
		debug_osd_text(ctx->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ctx->debug_osd));
		debug_osd_text(ctx->debug_osd, 2, "enc", "%ux%u h265",
			ctx->pipeline.width, ctx->pipeline.height);
		debug_osd_text(ctx->debug_osd, 3, "br", "%uk",
			__atomic_load_n(&ctx->live_bitrate, __ATOMIC_ACQUIRE));
		debug_osd_text(ctx->debug_osd, 4, "drop", "%llu",
			(unsigned long long)__atomic_load_n(&ctx->output_drops,
				__ATOMIC_RELAXED));
		debug_osd_end_frame(ctx->debug_osd);
	}
	if (!__atomic_load_n(&ctx->verbose, __ATOMIC_ACQUIRE))
		return;
	printf("> CV610 frames=%llu bytes=%llu output_drops=%llu\n",
		(unsigned long long)frames,
		(unsigned long long)__atomic_load_n(&ctx->bytes, __ATOMIC_RELAXED),
		(unsigned long long)__atomic_load_n(&ctx->output_drops,
			__ATOMIC_RELAXED));
	if (ctx->audio)
		printf("> CV610 audio frames=%llu bytes=%llu packets=%llu drops=%llu\n",
			(unsigned long long)audio_frames,
			(unsigned long long)audio_bytes,
			(unsigned long long)audio_packets,
			(unsigned long long)audio_drops);
}

static int cv610_run(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;
	int venc_fd = ss_mpi_venc_get_fd(CV610_VENC_CHN);

	if (venc_fd < 0)
		return -1;
	while (!cv610_pipeline_stop_requested()) {
		ot_venc_chn_status status;
		ot_venc_stream stream;
		fd_set readfds;
		struct timeval timeout = { 1, 0 };
		uint8_t *frame = NULL;
		size_t frame_len = 0;
		td_s32 ret;
		int ready;

		if (venc_api_get_reinit()) {
			venc_api_clear_reinit();
			venc_respawn_request();
			printf("> CV610 reinit requested: cold restart via fork+exec\n");
			break;
		}

		FD_ZERO(&readfds);
		FD_SET(venc_fd, &readfds);
		ready = select(venc_fd + 1, &readfds, NULL, NULL, &timeout);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return -1;
		if (ready == 0)
			continue;
		memset(&status, 0, sizeof(status));
		ret = ss_mpi_venc_query_status(CV610_VENC_CHN, &status);
		if (ret != TD_SUCCESS || status.cur_packs == 0)
			continue;
		memset(&stream, 0, sizeof(stream));
		stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
		if (!stream.pack)
			return -1;
		stream.pack_cnt = status.cur_packs;
		ret = ss_mpi_venc_get_stream(CV610_VENC_CHN, &stream, 1000);
		if (ret != TD_SUCCESS) {
			free(stream.pack);
			continue;
		}
		if (cv610_copy_stream(&stream, &frame, &frame_len) == 0) {
			int is_idr = cv610_frame_is_idr(frame, frame_len);

			if (ctx->frame_ring) {
				VencFrameMeta meta;

				memset(&meta, 0, sizeof(meta));
				meta.pts = stream.pack_cnt ? (uint32_t)stream.pack[0].pts : 0;
				meta.codec = VENC_FRAME_CODEC_H265;
				meta.flags = is_idr ? VENC_FRAME_FLAG_IDR : 0;
				if (venc_frame_ring_write(ctx->frame_ring, &meta,
					frame, (uint32_t)frame_len) != 0)
					__atomic_add_fetch(&ctx->output_drops, 1,
						__ATOMIC_RELAXED);
			} else if (ctx->socket_handle >= 0) {
				/* cv610_output_write owns per-datagram drop accounting. */
				(void)cv610_send_rtp_frame(ctx, frame, frame_len);
			}
			__atomic_add_fetch(&ctx->frames, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&ctx->bytes, frame_len, __ATOMIC_RELAXED);
			if (ctx->debug_osd)
				debug_osd_sample_cpu(ctx->debug_osd);
			cv610_report_frame_status(ctx);
			free(frame);
		}
		ret = ss_mpi_venc_release_stream(CV610_VENC_CHN, &stream);
		free(stream.pack);
		if (ret != TD_SUCCESS)
			return -1;
	}
	return 0;
}

static void cv610_teardown(void *opaque)
{
	Cv610RunnerContext *ctx = opaque;

	venc_httpd_pause();
	venc_httpd_stop();
	g_cv610_runner = NULL;
	cv610_audio_stop(ctx->audio);
	ctx->audio = NULL;
	cv610_output_stop(ctx);
	debug_osd_destroy(ctx->debug_osd);
	ctx->debug_osd = NULL;
	cv610_venc_stop(ctx);
	cv610_pipeline_stop();
}

static int cv610_map_result(int result)
{
	return result == 0 ? 0 : 2;
}

static const BackendOps g_cv610_ops = {
	.name = "cv610",
	.config_path = VENC_CONFIG_DEFAULT_PATH,
	.context_size = sizeof(Cv610RunnerContext),
	.config = cv610_config,
	.prepare = cv610_prepare,
	.init = cv610_init,
	.run = cv610_run,
	.teardown = cv610_teardown,
	.map_pipeline_result = cv610_map_result,
};

const BackendOps *cv610_runtime_backend_ops(void)
{
	return &g_cv610_ops;
}
