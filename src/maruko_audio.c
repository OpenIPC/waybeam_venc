#include "maruko_audio.h"

#include "audio_codec.h"
#include "maruko_ai_types.h"
#include "maruko_mi.h"
#include "output_socket.h"
#include "rtp_packetizer.h"
#include "star6e.h"  /* MI_SYS_ChnPort_t typedef + I6_SYS_MOD_AI */

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MARUKO_AI_DEFAULT_GAIN_DB   18

/* ── Output (UDP transport for encoded audio) ─────────────────────────── */

static void maruko_audio_output_reset(MarukoAudioOutput *ao)
{
	memset(ao, 0, sizeof(*ao));
	ao->socket_handle = -1;
	ao->cached_socket = -1;
}

static int maruko_audio_output_init(MarukoAudioOutput *ao,
	const MarukoOutput *video, uint16_t port_override,
	uint16_t max_payload)
{
	maruko_audio_output_reset(ao);
	if (!video)
		return -1;
	ao->video_output    = video;
	ao->port_override   = port_override;
	ao->max_payload_size = max_payload;

	if (port_override == 0)
		return 0;

	ao->socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (ao->socket_handle < 0) {
		fprintf(stderr, "[audio] ERROR: cannot create audio UDP socket\n");
		return -1;
	}

	if (output_socket_fill_udp_destination("127.0.0.1", port_override,
	    &ao->fallback_dst, &ao->fallback_dst_len) != 0) {
		close(ao->socket_handle);
		ao->socket_handle = -1;
		return -1;
	}
	return 0;
}

static int maruko_audio_resolve_target(MarukoAudioOutput *ao,
	int *out_socket, struct sockaddr_storage *out_dst, socklen_t *out_len)
{
	uint32_t gen;

	if (!ao || !ao->video_output)
		return -1;

	gen = __atomic_load_n(&ao->video_output->transport_gen,
		__ATOMIC_ACQUIRE);
	if (ao->cache_valid && ao->cached_gen == gen && !(gen & 1)) {
		*out_socket = ao->cached_socket;
		*out_dst    = ao->cached_dst;
		*out_len    = ao->cached_dst_len;
		return 0;
	}

	if (ao->port_override == 0) {
		/* Shared video target. */
		if (ao->video_output->socket_handle < 0 ||
		    ao->video_output->ring ||
		    ao->video_output->dst_len == 0)
			return -1;
		*out_socket = ao->video_output->socket_handle;
		*out_dst    = ao->video_output->dst;
		*out_len    = ao->video_output->dst_len;
	} else {
		struct sockaddr_in *udp_dst;

		if (ao->socket_handle < 0)
			return -1;
		*out_socket = ao->socket_handle;
		if (ao->video_output->transport == VENC_OUTPUT_URI_UDP &&
		    ao->video_output->dst_len == sizeof(struct sockaddr_in)) {
			memcpy(out_dst, &ao->video_output->dst, sizeof(*out_dst));
			*out_len = ao->video_output->dst_len;
			udp_dst = (struct sockaddr_in *)out_dst;
			udp_dst->sin_port = htons(ao->port_override);
		} else {
			memcpy(out_dst, &ao->fallback_dst, sizeof(*out_dst));
			*out_len = ao->fallback_dst_len;
		}
	}

	gen = __atomic_load_n(&ao->video_output->transport_gen,
		__ATOMIC_ACQUIRE);
	if (!(gen & 1)) {
		ao->cached_socket = *out_socket;
		ao->cached_dst    = *out_dst;
		ao->cached_dst_len = *out_len;
		ao->cached_gen    = gen;
		ao->cache_valid   = 1;
	}
	return 0;
}

static uint16_t maruko_audio_output_port(const MarukoAudioOutput *ao)
{
	const struct sockaddr_in *dst;

	if (!ao)
		return 0;
	if (ao->port_override != 0)
		return ao->port_override;
	if (!ao->video_output ||
	    ao->video_output->transport != VENC_OUTPUT_URI_UDP ||
	    ao->video_output->dst_len != sizeof(*dst))
		return 0;
	dst = (const struct sockaddr_in *)&ao->video_output->dst;
	return ntohs(dst->sin_port);
}

static void maruko_audio_output_teardown(MarukoAudioOutput *ao)
{
	if (!ao)
		return;
	if (ao->socket_handle >= 0 && ao->port_override != 0)
		close(ao->socket_handle);
	maruko_audio_output_reset(ao);
}

static int send_audio_rtp(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	struct {
		int socket;
		struct sockaddr_storage dst;
		socklen_t dst_len;
	} *target = opaque;
	if (!target)
		return -1;
	return output_socket_send_parts(target->socket, &target->dst,
		target->dst_len, 0 /* never connect()ed */,
		header, header_len, payload1, payload1_len,
		payload2, payload2_len);
}

static int maruko_audio_send(MarukoAudioOutput *ao, const uint8_t *data,
	size_t len, RtpPacketizerState *rtp, uint32_t frame_ticks,
	int rtp_mode)
{
	struct {
		int socket;
		struct sockaddr_storage dst;
		socklen_t dst_len;
	} target;

	if (!ao || !data || len == 0)
		return -1;
	if (maruko_audio_resolve_target(ao, &target.socket, &target.dst,
	    &target.dst_len) != 0)
		return -1;

	if (rtp_mode && rtp) {
		if (rtp_packetizer_send_packet(rtp, send_audio_rtp, &target,
		    data, len, NULL, 0, 0) != 0)
			return -1;
		rtp->timestamp += frame_ticks;
		return 0;
	}

	/* Compact (raw): tiny 4-byte preamble, mirrors Star6E format. */
	uint8_t hdr[4];
	uint16_t max_chunk = ao->max_payload_size > 4 ?
		(uint16_t)(ao->max_payload_size - 4) : 0;
	if (max_chunk == 0)
		return 0;

	size_t offset = 0;
	while (offset < len) {
		size_t chunk = len - offset;
		if (chunk > max_chunk)
			chunk = max_chunk;

		hdr[0] = 0xAA;
		hdr[1] = 0x01;
		hdr[2] = (uint8_t)((chunk >> 8) & 0xFF);
		hdr[3] = (uint8_t)(chunk & 0xFF);

		if (output_socket_send_parts(target.socket, &target.dst,
		    target.dst_len, 0, hdr, sizeof(hdr),
		    data + offset, chunk, NULL, 0) != 0)
			return -1;
		offset += chunk;
	}
	return 0;
}

/* ── Capture / encode threads ─────────────────────────────────────────── */

static void *capture_fn(void *arg)
{
	MarukoAudioState *st = arg;
	MARUKO_AI_Data_t mic, echo;
	struct timespec ts;
	uint64_t ts_us;

	while (st->running) {
		memset(&mic, 0, sizeof(mic));
		memset(&echo, 0, sizeof(echo));

		int ret = g_mi_ai.fnRead(st->ai_dev, st->chn_grp, &mic, &echo, 50);
		if (ret != 0)
			continue;

		if (mic.apvBuffer[0] && mic.u32Byte[0] > 0) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts_us = (uint64_t)ts.tv_sec * 1000000ULL +
				(uint64_t)ts.tv_nsec / 1000ULL;
			audio_ring_push(&st->cap_ring, mic.apvBuffer[0],
				(uint16_t)(mic.u32Byte[0] > 0xFFFF ? 0xFFFF
					: mic.u32Byte[0]),
				ts_us);
		}

		g_mi_ai.fnReleaseData(st->ai_dev, st->chn_grp, &mic, &echo);
	}
	return NULL;
}

static int video_output_is_rtp(const MarukoOutput *out)
{
	return out && out->transport == VENC_OUTPUT_URI_UDP && !out->ring;
}

static void *encode_fn(void *arg)
{
	MarukoAudioState *st = arg;
	uint8_t enc_buf[4096];
	int rtp_mode = video_output_is_rtp(st->output.video_output);

	if (st->verbose)
		printf("[audio] Started (rate=%u, ch=%u, codec=%d, rtp=%d)\n",
			st->sample_rate, st->channels, st->codec_type, rtp_mode);

	while (st->running || audio_ring_count(&st->cap_ring) > 0) {
		AudioRingEntry entry;
		const uint8_t *data;
		size_t len;

		if (!audio_ring_pop_wait(&st->cap_ring, &entry, 25))
			continue;

		data = entry.pcm;
		len  = entry.length;

		if (st->rec_ring)
			audio_ring_push(st->rec_ring, data, (uint16_t)len,
				entry.timestamp_us);

		if (len > 0 && st->codec_type == AUDIO_CODEC_TYPE_OPUS &&
		    st->opus.encoder && st->opus.encode) {
			int frame_size = (int)(len / 2 / st->channels);
			int32_t encoded = st->opus.encode(st->opus.encoder,
				(const int16_t *)data, frame_size,
				enc_buf, (int32_t)sizeof(enc_buf));
			if (encoded <= 0) {
				fprintf(stderr,
					"[audio] opus_encode error %d, dropping frame\n",
					(int)encoded);
				continue;
			}
			data = enc_buf;
			len  = (size_t)encoded;
		} else if (len > 0 &&
		           (st->codec_type == AUDIO_CODEC_TYPE_G711A ||
		            st->codec_type == AUDIO_CODEC_TYPE_G711U)) {
			size_t n = len / 2;
			if (n > sizeof(enc_buf))
				n = sizeof(enc_buf);
			len = audio_codec_encode_g711((const int16_t *)data, n,
				enc_buf, st->codec_type);
			data = enc_buf;
		} else if (len > 0 &&
		           st->codec_type == AUDIO_CODEC_TYPE_RAW &&
		           rtp_mode) {
			/* RFC 3551: L16 in network byte order. */
			size_t pairs = (len & ~(size_t)1) / 2;
			if (pairs > sizeof(enc_buf) / 2)
				pairs = sizeof(enc_buf) / 2;
			for (size_t i = 0; i < pairs; i++) {
				enc_buf[2 * i]     = data[2 * i + 1];
				enc_buf[2 * i + 1] = data[2 * i];
			}
			data = enc_buf;
			len  = pairs * 2;
		}

		if (len > 0)
			(void)maruko_audio_send(&st->output, data, len,
				&st->rtp, st->rtp_frame_ticks, rtp_mode);
	}

	if (st->verbose)
		printf("[audio] Stopped\n");
	return NULL;
}

/* ── Bring-up helpers ─────────────────────────────────────────────────── */

static int volume_to_db(int volume)
{
	if (volume <= 0)   return -60;
	if (volume >= 100) return 30;
	return -60 + (volume * 90) / 100;
}

static int open_ai_device(MarukoAudioState *st, int volume, int mute)
{
	MARUKO_AI_Attr_t attr;
	int ifaces[1] = { st->interface };
	int8_t gain = MARUKO_AI_DEFAULT_GAIN_DB;
	int ret;

	memset(&attr, 0, sizeof(attr));
	attr.enFormat      = E_MI_AUDIO_FORMAT_PCM_S16_LE;
	attr.enSoundMode   = (st->channels >= 2) ?
		E_MI_AUDIO_SOUND_MODE_STEREO : E_MI_AUDIO_SOUND_MODE_MONO;
	attr.enSampleRate  = (int32_t)st->sample_rate;
	attr.u32PeriodSize = st->period_size;
	attr.bInterleaved  = 1;

	ret = g_mi_ai.fnOpen(st->ai_dev, &attr);
	if (ret != 0) {
		fprintf(stderr, "[audio] MI_AI_Open failed: 0x%x\n", ret);
		return -1;
	}
	st->device_opened = 1;

	ret = g_mi_ai.fnAttachIf(st->ai_dev, ifaces, 1);
	if (ret != 0) {
		fprintf(stderr, "[audio] MI_AI_AttachIf(if=%d) failed: 0x%x\n",
			st->interface, ret);
		return -1;
	}

	if (g_mi_ai.fnSetIfGain)
		(void)g_mi_ai.fnSetIfGain(st->interface, gain, gain);
	(void)g_mi_ai.fnSetGain(st->ai_dev, st->chn_grp, &gain, 1);

	{
		int mutes[1] = { mute ? 1 : 0 };
		(void)g_mi_ai.fnSetMute(st->ai_dev, st->chn_grp, mutes, 1);
	}

	{
		MI_SYS_ChnPort_t port = {
			.module  = I6_SYS_MOD_AI,
			.device  = (uint32_t)st->ai_dev,
			.channel = st->chn_grp,
			.port    = 0,
		};
		(void)MI_SYS_SetChnOutputPortDepth(&port, 3, 5);
	}

	ret = g_mi_ai.fnEnableChnGroup(st->ai_dev, st->chn_grp);
	if (ret != 0) {
		fprintf(stderr, "[audio] MI_AI_EnableChnGroup failed: 0x%x\n", ret);
		return -1;
	}
	st->group_enabled = 1;

	(void)volume;  /* gain table is per-IF; volume mapping reserved for future */
	return 0;
}

static void disable_ai_device(MarukoAudioState *st)
{
	if (st->group_enabled) {
		g_mi_ai.fnDisableChnGroup(st->ai_dev, st->chn_grp);
		st->group_enabled = 0;
	}
	if (st->device_opened) {
		g_mi_ai.fnClose(st->ai_dev);
		st->device_opened = 0;
	}
}

static int start_threads(MarukoAudioState *st)
{
	audio_ring_init(&st->cap_ring);
	st->running = 1;

	if (pthread_create(&st->capture_thread, NULL, capture_fn, st) != 0) {
		fprintf(stderr, "[audio] capture thread create failed\n");
		st->running = 0;
		audio_ring_destroy(&st->cap_ring);
		return -1;
	}
	{
		struct sched_param sp = { .sched_priority = 1 };
		if (pthread_setschedparam(st->capture_thread, SCHED_FIFO,
		    &sp) != 0 && st->verbose)
			fprintf(stderr, "[audio] note: SCHED_FIFO unavailable "
				"(run as root?)\n");
	}

	if (pthread_create(&st->encode_thread, NULL, encode_fn, st) != 0) {
		fprintf(stderr, "[audio] encode thread create failed\n");
		st->running = 0;
		audio_ring_wake(&st->cap_ring);
		pthread_join(st->capture_thread, NULL);
		audio_ring_destroy(&st->cap_ring);
		return -1;
	}

	st->started = 1;
	return 0;
}

/* ── Public lifecycle ─────────────────────────────────────────────────── */

int maruko_audio_init(MarukoAudioState *state, const MarukoBackendConfig *cfg,
	const MarukoOutput *output)
{
	if (!state)
		return 0;

	memset(state, 0, sizeof(*state));
	maruko_audio_output_reset(&state->output);

	if (!cfg || !cfg->audio.enabled || !output)
		return 0;

	if (!g_mi_ai.handle || !g_mi_ai.fnOpen) {
		fprintf(stderr,
			"[audio] WARNING: libmi_ai.so not loaded — audio disabled\n");
		return 0;
	}

	state->sample_rate = cfg->audio.sample_rate ? cfg->audio.sample_rate : 8000;
	state->channels    = cfg->audio.channels ? cfg->audio.channels : 1;
	state->verbose     = cfg->verbose;
	state->codec_type  = audio_codec_parse_name(cfg->audio.codec);
	state->ai_dev      = 0;
	state->chn_grp     = 0;
	state->interface   = MARUKO_AI_IF_ADC_AB;
	/* ~20ms per period at any rate. */
	state->period_size = state->sample_rate / 50;

	if (state->codec_type == AUDIO_CODEC_TYPE_OPUS &&
	    audio_codec_opus_init(&state->opus, state->sample_rate,
	    state->channels) != 0)
		state->codec_type = AUDIO_CODEC_TYPE_RAW;

	audio_codec_stdout_filter_start();
	state->lib_loaded = 1;

	if (open_ai_device(state, cfg->audio.volume, cfg->audio.mute) != 0)
		goto fail;

	if (maruko_audio_output_init(&state->output, output,
	    cfg->audio_port, cfg->max_payload_size) != 0)
		goto fail;

	if (video_output_is_rtp(output)) {
		state->rtp.seq       = (uint16_t)(rand() & 0xFFFF);
		state->rtp.timestamp = (uint32_t)rand();
		state->rtp.ssrc      = ((uint32_t)rand() << 16) ^
			(uint32_t)rand() ^ 0xA0D1DEAD;
		switch (state->codec_type) {
		case AUDIO_CODEC_TYPE_G711U:
			state->rtp.payload_type = (state->sample_rate == 8000) ? 0 : 112;
			break;
		case AUDIO_CODEC_TYPE_G711A:
			state->rtp.payload_type = (state->sample_rate == 8000) ? 8 : 113;
			break;
		case AUDIO_CODEC_TYPE_OPUS:
			state->rtp.payload_type = 98;
			break;
		default:
			state->rtp.payload_type = (state->sample_rate == 44100) ? 11 : 110;
			break;
		}
		state->rtp_frame_ticks = (state->codec_type == AUDIO_CODEC_TYPE_OPUS)
			? (AUDIO_CODEC_OPUS_RTP_CLOCK_HZ / 50)
			: (state->sample_rate / 50);
	}

	if (output->transport == VENC_OUTPUT_URI_UDP &&
	    maruko_audio_output_port(&state->output) == 0)
		fprintf(stderr,
			"[audio] WARNING: audio output has no destination port\n");

	if (start_threads(state) != 0)
		goto fail;

	if (state->verbose) {
		uint16_t port = maruko_audio_output_port(&state->output);
		printf("[audio] Initialized: %s @ %u Hz, %u ch, port %u\n",
			cfg->audio.codec, state->sample_rate, state->channels,
			port);
	}
	return 0;

fail:
	audio_codec_opus_teardown(&state->opus);
	disable_ai_device(state);
	maruko_audio_output_teardown(&state->output);
	if (state->lib_loaded) {
		audio_codec_stdout_filter_stop();
		state->lib_loaded = 0;
	}
	fprintf(stderr,
		"[audio] WARNING: audio init failed, continuing without audio\n");
	return 0;
}

void maruko_audio_teardown(MarukoAudioState *state)
{
	if (!state)
		return;

	if (state->started) {
		state->running = 0;
		audio_ring_wake(&state->cap_ring);
		pthread_join(state->capture_thread, NULL);
		pthread_join(state->encode_thread, NULL);
		if (state->cap_ring.dropped)
			fprintf(stderr,
				"[audio] cap_ring dropped %u frames\n",
				state->cap_ring.dropped);
		audio_ring_destroy(&state->cap_ring);
		state->started = 0;
	}

	audio_codec_opus_teardown(&state->opus);
	disable_ai_device(state);
	maruko_audio_output_teardown(&state->output);

	if (state->lib_loaded) {
		audio_codec_stdout_filter_stop();
		state->lib_loaded = 0;
	}
}

int maruko_audio_apply_mute(MarukoAudioState *state, int muted)
{
	int mutes[1];
	int ret;

	if (!state || !state->group_enabled || !g_mi_ai.fnSetMute)
		return -1;

	mutes[0] = muted ? 1 : 0;
	ret = g_mi_ai.fnSetMute(state->ai_dev, state->chn_grp, mutes, 1);
	if (ret != 0) {
		fprintf(stderr, "[audio] SetMute(%d) failed: 0x%x\n",
			muted ? 1 : 0, ret);
		return -1;
	}
	return 0;
}
