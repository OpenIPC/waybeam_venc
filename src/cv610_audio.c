/*
 * CV610 analog input -> inner ACODEC -> AI -> vendor AENC/Opus -> RTP.
 *
 * The hardware path and its ordering intentionally match the standalone
 * streamer that passed the CV610 audio bring-up gates. The only integration
 * change is the transport edge: encoded Opus frames use Waybeam's shared RTP
 * packetizer and output-socket helpers.
 */

#include "cv610_audio.h"

#include "output_socket.h"
#include "rtp_packetizer.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "ot_acodec.h"
#include "ot_common.h"
#include "ot_common_aenc.h"
#include "ot_common_aio.h"
#include "ss_audio_opus_adp.h"
#include "ss_mpi_audio.h"
#include "ss_mpi_sys_bind.h"

#define CV610_AUDIO_DEV 0
#define CV610_AI_CHN 0
#define CV610_AENC_CHN 0
#define CV610_ACODEC_DEVICE "/dev/acodec"
#define CV610_AUDIO_SAMPLE_RATE 48000u
#define CV610_AUDIO_POINT_NUM 480u
#define CV610_AUDIO_BITRATE 32000u
#define CV610_AUDIO_PAYLOAD_TYPE 98u
#define CV610_AUDIO_MIC_GAIN 8u
#define CV610_ACODEC_POWERUP_TRIES 20
#define CV610_ACODEC_POWERUP_STEP_US 100000

struct Cv610AudioState {
	int acodec_fd;
	int aenc_fd;
	int socket_handle;
	struct sockaddr_storage destination;
	socklen_t destination_len;
	VencOutputUriType transport;
	int connected_udp;
	RtpPacketizerState rtp;
	uint32_t ticks_per_frame;
	pthread_t thread;
	pthread_mutex_t stats_lock;
	int running;
	int thread_started;
	int audio_initialized;
	int ai_enabled;
	int ai_chn_enabled;
	int aenc_opus_initialized;
	int aenc_created;
	int bound;
	uint64_t frames;
	uint64_t bytes;
	uint64_t packets;
	uint64_t drops;
};

#define AUDIO_CHECK(expr) do { \
	td_s32 check_ret = (expr); \
	if (check_ret != TD_SUCCESS) { \
		fprintf(stderr, "ERROR: %s=0x%x\n", #expr, check_ret); \
		return -1; \
	} \
} while (0)

static int cv610_acodec_apply(int fd, const char *name,
	unsigned long request, td_u32 value)
{
	if (ioctl(fd, request, &value) == 0)
		return 0;
	fprintf(stderr, "ERROR: ACODEC %s: %s\n", name, strerror(errno));
	return -1;
}

static int cv610_acodec_reset(Cv610AudioState *state)
{
	state->acodec_fd = open(CV610_ACODEC_DEVICE, O_RDWR);
	if (state->acodec_fd < 0) {
		fprintf(stderr, "ERROR: open %s: %s (is open_acodec loaded?)\n",
			CV610_ACODEC_DEVICE, strerror(errno));
		return -1;
	}
	if (ioctl(state->acodec_fd, OT_ACODEC_SOFT_RESET_CTRL) == 0)
		return 0;
	fprintf(stderr, "ERROR: ACODEC reset: %s\n", strerror(errno));
	return -1;
}

static int cv610_ai_set_attr(Cv610AudioState *state)
{
	ot_aio_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.sample_rate = (ot_audio_sample_rate)CV610_AUDIO_SAMPLE_RATE;
	attr.bit_width = OT_AUDIO_BIT_WIDTH_16;
	attr.work_mode = OT_AIO_MODE_I2S_MASTER;
	attr.snd_mode = OT_AUDIO_SOUND_MODE_MONO;
	attr.frame_num = 8;
	attr.point_num_per_frame = CV610_AUDIO_POINT_NUM;
	attr.chn_cnt = 1;
	attr.clk_share = 1;
	attr.i2s_type = OT_AIO_I2STYPE_INNERCODEC;
	AUDIO_CHECK(ss_mpi_ai_set_pub_attr(CV610_AUDIO_DEV, &attr));
	return 0;
}

static int cv610_ai_enable(Cv610AudioState *state)
{
	ot_ai_chn_param param;

	AUDIO_CHECK(ss_mpi_ai_enable(CV610_AUDIO_DEV));
	state->ai_enabled = 1;
	memset(&param, 0, sizeof(param));
	param.usr_frame_depth = 4;
	AUDIO_CHECK(ss_mpi_ai_set_chn_param(CV610_AUDIO_DEV,
		CV610_AI_CHN, &param));
	AUDIO_CHECK(ss_mpi_ai_enable_chn(CV610_AUDIO_DEV, CV610_AI_CHN));
	state->ai_chn_enabled = 1;
	return 0;
}

static int cv610_acodec_wait_for_power(Cv610AudioState *state)
{
	td_u32 fs = OT_ACODEC_FS_48000;
	int i;

	for (i = 0; i < CV610_ACODEC_POWERUP_TRIES; ++i) {
		if (ioctl(state->acodec_fd, OT_ACODEC_SET_I2S1_FS, &fs) == 0) {
			if (i > 0)
				printf("> CV610 ACODEC powered up after %d ms\n",
					i * (CV610_ACODEC_POWERUP_STEP_US / 1000));
			return 0;
		}
		if (errno != EPERM)
			break;
		usleep(CV610_ACODEC_POWERUP_STEP_US);
	}
	fprintf(stderr, "ERROR: ACODEC set-fs: %s (codec never powered up)\n",
		strerror(errno));
	return -1;
}

static int cv610_acodec_configure(Cv610AudioState *state, int muted)
{
	td_u32 gain = muted ? 0u : CV610_AUDIO_MIC_GAIN;

	if (cv610_acodec_wait_for_power(state) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "set-mixer",
			OT_ACODEC_SET_MIXER_MIC, OT_ACODEC_MIXER_IN0) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "boost-l",
			OT_ACODEC_ENABLE_BOOSTL, 1) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "boost-r",
			OT_ACODEC_ENABLE_BOOSTR, 1) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "gain-micl",
			OT_ACODEC_SET_GAIN_MICL, gain) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "gain-micr",
			OT_ACODEC_SET_GAIN_MICR, gain) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "adc-hpf",
			OT_ACODEC_SET_ADC_HP_FILTER, 1) != 0)
		return -1;
	printf("> CV610 ACODEC 48000 Hz IN0 boost=1 gain=%u hpf=1\n", gain);
	return 0;
}

static int cv610_aenc_start(Cv610AudioState *state)
{
	ot_aenc_attr_opus opus;
	ot_aenc_chn_attr attr;
	ot_mpp_chn source;
	ot_mpp_chn destination;

	AUDIO_CHECK(ss_mpi_aenc_opus_init());
	state->aenc_opus_initialized = 1;
	memset(&opus, 0, sizeof(opus));
	opus.sample_rate = (ot_audio_sample_rate)CV610_AUDIO_SAMPLE_RATE;
	opus.bit_width = OT_AUDIO_BIT_WIDTH_16;
	opus.snd_mode = OT_AUDIO_SOUND_MODE_MONO;
	opus.bit_rate = (ot_opus_bps)CV610_AUDIO_BITRATE;
	opus.app = OT_OPUS_APPLICATION_RESTRICTED_LOWDELAY;
	memset(&attr, 0, sizeof(attr));
	attr.type = OT_PT_OPUS;
	attr.point_num_per_frame = CV610_AUDIO_POINT_NUM;
	attr.buf_size = 8;
	attr.value = &opus;
	AUDIO_CHECK(ss_mpi_aenc_create_chn(CV610_AENC_CHN, &attr));
	state->aenc_created = 1;
	source.mod_id = OT_ID_AI;
	source.dev_id = CV610_AUDIO_DEV;
	source.chn_id = CV610_AI_CHN;
	destination.mod_id = OT_ID_AENC;
	destination.dev_id = 0;
	destination.chn_id = CV610_AENC_CHN;
	AUDIO_CHECK(ss_mpi_sys_bind(&source, &destination));
	state->bound = 1;
	printf("> CV610 AENC Opus 32000 bit/s restricted-low-delay, 10.0 ms frames\n");
	return 0;
}

static int cv610_audio_output_start(Cv610AudioState *state,
	const VencConfig *config, const VencOutputUri *video_output)
{
	VencOutputUri audio_output;
	uint16_t port;

	if (config->outgoing.audio_port < 0)
		return 0;
	memset(&audio_output, 0, sizeof(audio_output));
	audio_output.type = VENC_OUTPUT_URI_UDP;
	if (video_output && video_output->type == VENC_OUTPUT_URI_UDP)
		snprintf(audio_output.host, sizeof(audio_output.host), "%s",
			video_output->host);
	else
		snprintf(audio_output.host, sizeof(audio_output.host), "127.0.0.1");
	port = config->outgoing.audio_port == 0 && video_output &&
		video_output->type == VENC_OUTPUT_URI_UDP ? video_output->port :
		(uint16_t)config->outgoing.audio_port;
	if (port == 0) {
		fprintf(stderr, "ERROR: CV610 audio has no UDP destination port\n");
		return -1;
	}
	audio_output.port = port;
	if (output_socket_configure(&state->socket_handle, &state->destination,
		&state->destination_len, &state->transport, &audio_output,
		config->outgoing.connected_udp, 0, &state->connected_udp) != 0)
		return -1;
	printf("> CV610 audio RTP udp://%s:%u PT=%u clock=48000\n",
		audio_output.host, port, CV610_AUDIO_PAYLOAD_TYPE);
	return 0;
}

static int cv610_audio_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	Cv610AudioState *state = opaque;

	return output_socket_send_parts(state->socket_handle,
		&state->destination, state->destination_len, state->connected_udp,
		header, header_len, payload1, payload1_len, payload2, payload2_len);
}

static void cv610_audio_note_frame(Cv610AudioState *state, size_t bytes,
	int sent)
{
	pthread_mutex_lock(&state->stats_lock);
	state->frames++;
	state->bytes += bytes;
	if (sent)
		state->packets++;
	else
		state->drops++;
	pthread_mutex_unlock(&state->stats_lock);
}

static void *cv610_audio_thread(void *opaque)
{
	Cv610AudioState *state = opaque;
	int first = 1;

	while (__atomic_load_n(&state->running, __ATOMIC_ACQUIRE)) {
		struct timeval timeout = { 1, 0 };
		ot_audio_stream stream;
		fd_set readfds;
		td_s32 ret;
		int ready;

		FD_ZERO(&readfds);
		FD_SET(state->aenc_fd, &readfds);
		ready = select(state->aenc_fd + 1, &readfds, NULL, NULL, &timeout);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			fprintf(stderr, "ERROR: select AENC: %s\n", strerror(errno));
			break;
		}
		if (ready == 0) {
			fprintf(stderr, "WARNING: AENC timeout waiting for audio frame\n");
			continue;
		}
		memset(&stream, 0, sizeof(stream));
		ret = ss_mpi_aenc_get_stream(CV610_AENC_CHN, &stream, 0);
		if (ret != TD_SUCCESS)
			continue;
		if (stream.len > 0 && stream.stream) {
			int sent = 0;

			if (state->socket_handle < 0) {
				sent = 1;
			} else if (rtp_packetizer_send_packet(&state->rtp,
				cv610_audio_write, state, stream.stream, stream.len,
				NULL, 0, first) == 0) {
				sent = 1;
			}
			/* RTP time follows capture cadence even when transport drops a
			 * packet; sequence numbers advance only for emitted packets. */
			state->rtp.timestamp += state->ticks_per_frame;
			cv610_audio_note_frame(state, stream.len, sent);
			if (sent)
				first = 0;
		}
		ret = ss_mpi_aenc_release_stream(CV610_AENC_CHN, &stream);
		if (ret != TD_SUCCESS)
			break;
	}
	return NULL;
}

Cv610AudioState *cv610_audio_start(const VencConfig *config,
	const VencOutputUri *video_output)
{
	Cv610AudioState *state;
	struct timespec now;

	if (!config || !config->audio.enabled)
		return NULL;
	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;
	state->acodec_fd = -1;
	state->aenc_fd = -1;
	state->socket_handle = -1;
	if (pthread_mutex_init(&state->stats_lock, NULL) != 0)
		goto fail;
	if (ss_mpi_audio_init() != TD_SUCCESS) {
		fprintf(stderr, "ERROR: ss_mpi_audio_init failed\n");
		goto fail_mutex;
	}
	state->audio_initialized = 1;
	/* Load-bearing order, established by the standalone hardware bring-up. */
	if (cv610_acodec_reset(state) != 0 || cv610_ai_set_attr(state) != 0 ||
		cv610_ai_enable(state) != 0 ||
		cv610_acodec_configure(state, config->audio.mute) != 0 ||
		cv610_aenc_start(state) != 0 ||
		cv610_audio_output_start(state, config, video_output) != 0)
		goto fail_started;
	clock_gettime(CLOCK_MONOTONIC, &now);
	state->rtp.seq = (uint16_t)(now.tv_nsec ^ getpid());
	state->rtp.timestamp = (uint32_t)(now.tv_nsec ^
		(now.tv_sec * CV610_AUDIO_SAMPLE_RATE));
	state->rtp.ssrc = (uint32_t)(now.tv_nsec ^ (getpid() << 8) ^
		(now.tv_sec + 1));
	state->rtp.payload_type = CV610_AUDIO_PAYLOAD_TYPE;
	state->ticks_per_frame = CV610_AUDIO_POINT_NUM;
	state->aenc_fd = ss_mpi_aenc_get_fd(CV610_AENC_CHN);
	if (state->aenc_fd < 0) {
		fprintf(stderr, "ERROR: ss_mpi_aenc_get_fd=0x%x\n", state->aenc_fd);
		goto fail_started;
	}
	__atomic_store_n(&state->running, 1, __ATOMIC_RELEASE);
	if (pthread_create(&state->thread, NULL, cv610_audio_thread, state) != 0) {
		__atomic_store_n(&state->running, 0, __ATOMIC_RELEASE);
		fprintf(stderr, "ERROR: create CV610 audio thread failed\n");
		goto fail_started;
	}
	state->thread_started = 1;
	return state;

fail_started:
	cv610_audio_stop(state);
	return NULL;
fail_mutex:
	pthread_mutex_destroy(&state->stats_lock);
fail:
	free(state);
	return NULL;
}

void cv610_audio_stop(Cv610AudioState *state)
{
	ot_mpp_chn source = { OT_ID_AI, CV610_AUDIO_DEV, CV610_AI_CHN };
	ot_mpp_chn destination = { OT_ID_AENC, 0, CV610_AENC_CHN };

	if (!state)
		return;
	if (state->thread_started) {
		__atomic_store_n(&state->running, 0, __ATOMIC_RELEASE);
		pthread_join(state->thread, NULL);
	}
	if (state->bound)
		(void)ss_mpi_sys_unbind(&source, &destination);
	if (state->aenc_created)
		(void)ss_mpi_aenc_destroy_chn(CV610_AENC_CHN);
	if (state->aenc_opus_initialized)
		(void)ss_mpi_aenc_opus_deinit();
	if (state->ai_chn_enabled)
		(void)ss_mpi_ai_disable_chn(CV610_AUDIO_DEV, CV610_AI_CHN);
	if (state->ai_enabled)
		(void)ss_mpi_ai_disable(CV610_AUDIO_DEV);
	if (state->acodec_fd >= 0)
		close(state->acodec_fd);
	if (state->audio_initialized)
		(void)ss_mpi_audio_exit();
	if (state->socket_handle >= 0)
		close(state->socket_handle);
	pthread_mutex_destroy(&state->stats_lock);
	free(state);
}

void cv610_audio_get_stats(Cv610AudioState *state, uint64_t *frames,
	uint64_t *bytes, uint64_t *packets, uint64_t *drops)
{
	if (!state)
		return;
	pthread_mutex_lock(&state->stats_lock);
	if (frames)
		*frames = state->frames;
	if (bytes)
		*bytes = state->bytes;
	if (packets)
		*packets = state->packets;
	if (drops)
		*drops = state->drops;
	pthread_mutex_unlock(&state->stats_lock);
}
