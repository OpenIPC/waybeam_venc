#include "star6e_audio.h"

#include "audio_codec.h"
#include "star6e.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct Star6eAudioDevConfig {
	int rate;
	int bit24On;
	int intf;
	int sound;
	unsigned int frmNum;
	unsigned int packNumPerFrm;
	unsigned int codecChnNum;
	unsigned int chnNum;
	struct {
		int leftJustOn;
		int clock;
		char syncRxClkOn;
		unsigned int tdmSlotNum;
		int bit24On;
	} i2s;
};

struct Star6eAudioFrame {
	int bit24On;
	int sound;
	unsigned char *addr[16];
	unsigned long long timestamp;
	unsigned int sequence;
	unsigned int length;
	unsigned int poolId[2];
	unsigned char *pcmAddr[16];
	unsigned int pcmLength;
};

/* MI_SYS_ConfigPrivateMMAPool is intentionally not called: on MMU-enabled
 * platforms (including SSC30KQ) the kernel unconditionally disables private
 * MMA pools and prints a kernel warning on every call regardless of
 * parameters.  Audio DMA uses the shared pool without measurable impact. */

/* Local aliases over the shared codec types.  Existing code reads better
 * with the AUDIO_TYPE_* names than with AUDIO_CODEC_TYPE_*, and they sit
 * inline in many switch tests. */
enum {
	AUDIO_TYPE_G711A = AUDIO_CODEC_TYPE_G711A,
	AUDIO_TYPE_G711U = AUDIO_CODEC_TYPE_G711U,
	AUDIO_TYPE_OPUS  = AUDIO_CODEC_TYPE_OPUS,
};
#define OPUS_RTP_CLOCK_HZ AUDIO_CODEC_OPUS_RTP_CLOCK_HZ

/* Backwards-compatible alias for video code that calls into the filter. */
int stdout_filter_real_fd(void)
{
	return audio_codec_stdout_filter_real_fd();
}

/* Persists AI device state across reinit cycles.  After removing MI_SYS_Exit
 * from the reinit path, the kernel AI device/channel state survives pipeline
 * stop/start.  Re-initializing it triggers a CamOsMutexLock deadlock after
 * 2+ VPE create/destroy cycles. */
static struct {
	int initialized;
	Star6eAudioLib lib;
} g_ai_persist;

static void star6e_audio_teardown_opus(Star6eAudioState *state);

static int star6e_audio_lib_load(Star6eAudioLib *lib)
{
	memset(lib, 0, sizeof(*lib));
	lib->handle = dlopen("libmi_ai.so", RTLD_NOW | RTLD_GLOBAL);

	if (!lib->handle) {
		fprintf(stderr, "[audio] Cannot load libmi_ai.so: %s\n", dlerror());
		return -1;
	}

#define LOAD_SYM(field, name) do { \
	lib->field = dlsym(lib->handle, name); \
	if (!lib->field) { \
		fprintf(stderr, "[audio] Missing symbol: %s\n", name); \
		dlclose(lib->handle); \
		memset(lib, 0, sizeof(*lib)); \
		return -1; \
	} \
} while (0)

	LOAD_SYM(fnDisableDevice, "MI_AI_Disable");
	LOAD_SYM(fnEnableDevice, "MI_AI_Enable");
	LOAD_SYM(fnSetDeviceConfig, "MI_AI_SetPubAttr");
	LOAD_SYM(fnDisableChannel, "MI_AI_DisableChn");
	LOAD_SYM(fnEnableChannel, "MI_AI_EnableChn");
	LOAD_SYM(fnSetMute, "MI_AI_SetMute");
	LOAD_SYM(fnSetVolume, "MI_AI_SetVqeVolume");
	LOAD_SYM(fnFreeFrame, "MI_AI_ReleaseFrame");
	LOAD_SYM(fnGetFrame, "MI_AI_GetFrame");

#undef LOAD_SYM
	return 0;
}

static void star6e_audio_lib_unload(Star6eAudioLib *lib)
{
	if (lib->handle)
		dlclose(lib->handle);
	memset(lib, 0, sizeof(*lib));
}

static int star6e_audio_volume_to_db(int volume)
{
	if (volume <= 0)
		return -60;
	if (volume >= 100)
		return 30;
	return -60 + (volume * 90) / 100;
}

/* Thread A: audio capture only.
 * GetFrame → timestamp → push raw PCM to cap_ring → ReleaseFrame.
 * Nothing else: no encode, no send, no prints in the hot loop.
 * Runs SCHED_FIFO so it is never preempted past one 20ms DMA frame. */
static void *star6e_audio_capture_fn(void *arg)
{
	Star6eAudioState *state = arg;
	Star6eAudioFrame frame;

	while (state->running) {
		int ret;
		struct timespec ts;
		uint64_t ts_us;

		memset(&frame, 0, sizeof(frame));
		ret = state->lib.fnGetFrame(0, 0, &frame, NULL, 50);
		if (ret != 0)
			continue;

		if (frame.addr[0] && frame.length > 0) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts_us = (uint64_t)ts.tv_sec * 1000000ULL +
				(uint64_t)ts.tv_nsec / 1000ULL;
			audio_ring_push(&state->cap_ring, frame.addr[0],
				(uint16_t)(frame.length > 0xFFFF ? 0xFFFF
					: frame.length),
				ts_us);
		}

		state->lib.fnFreeFrame(0, 0, &frame, NULL);
	}
	return NULL;
}

/* Thread B: encode and send.
 * Pops raw PCM from cap_ring, pushes to rec_ring (recording), encodes,
 * and sends via RTP/UDP.  Runs at normal SCHED_OTHER priority — timing
 * here does not affect the DMA capture window. */
static void *star6e_audio_encode_fn(void *arg)
{
	Star6eAudioState *state = arg;
	uint8_t enc_buf[4096];

	if (state->verbose)
		printf("[audio] Started (rate=%u, ch=%u, codec=%d)\n",
			state->sample_rate, state->channels, state->codec_type);

	while (state->running || audio_ring_count(&state->cap_ring) > 0) {
		AudioRingEntry entry;
		const uint8_t *data;
		size_t len;

		if (!audio_ring_pop_wait(&state->cap_ring, &entry, 25))
			continue;

		data = entry.pcm;
		len  = entry.length;

		/* Forward raw PCM to recording ring before encode */
		if (state->rec_ring)
			audio_ring_push(state->rec_ring, data,
				(uint16_t)len, entry.timestamp_us);

		if (len > 0 && state->codec_type == AUDIO_TYPE_OPUS &&
		    state->opus.encoder && state->opus.encode) {
			int frame_size = (int)(len / 2 / state->channels);
			int32_t encoded = state->opus.encode(state->opus.encoder,
				(const int16_t *)data, frame_size,
				enc_buf, (int32_t)sizeof(enc_buf));
			if (encoded <= 0) {
				fprintf(stderr, "[audio] opus_encode error %d, dropping frame\n",
					(int)encoded);
				continue;
			}
			data = enc_buf;
			len = (size_t)encoded;
		} else if (len > 0 &&
		           (state->codec_type == AUDIO_TYPE_G711A ||
		            state->codec_type == AUDIO_TYPE_G711U)) {
			size_t n = len / 2;
			if (n > sizeof(enc_buf)) n = sizeof(enc_buf);
			len = audio_codec_encode_g711((const int16_t *)data, n,
				enc_buf, state->codec_type);
			data = enc_buf;
		} else if (len > 0 && state->codec_type < 0 &&
		           star6e_output_is_rtp(state->output.video_output)) {
			/* RFC 3551: L16 samples are carried in network byte order.
			 * The MI_AI pipeline yields native (little-endian) S16, so
			 * swap into enc_buf before RTP packetization. */
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
			(void)star6e_audio_output_send(&state->output, data, len,
				&state->rtp, state->rtp_frame_ticks);
	}

	if (state->verbose)
		printf("[audio] Stopped\n");
	return NULL;
}

static int configure_ai_device(Star6eAudioState *state)
{
	Star6eAudioDevConfig dev_cfg;
	int ret;

	memset(&dev_cfg, 0, sizeof(dev_cfg));
	dev_cfg.rate = (int)state->sample_rate;
	dev_cfg.intf = 0;
	dev_cfg.sound = (state->channels >= 2) ? 1 : 0;
	/* 20 frames = 400ms DMA ring buffer.  Prevents data loss under ISP/AE
	 * preemption. */
	dev_cfg.frmNum = 20;
	/* Scale frame size to maintain ~20ms per frame at any sample rate */
	dev_cfg.packNumPerFrm = (unsigned int)(state->sample_rate / 50);
	dev_cfg.codecChnNum = 0;
	dev_cfg.chnNum = state->channels;
	/* SDK reference (audio_all_test_case.c): MCLK disabled (clock=0); the
	 * I2S master generates its own clock from internal PLL based on rate.
	 * bSyncClock=1: I2S TX and RX share clock source. */
	dev_cfg.i2s.clock = 0;
	dev_cfg.i2s.syncRxClkOn = 1;

	ret = state->lib.fnSetDeviceConfig(0, &dev_cfg);
	if (ret != 0) {
		fprintf(stderr, "[audio] ERROR: SetDeviceConfig failed (%d)\n", ret);
		return -1;
	}
	return 0;
}

static int start_ai_capture(Star6eAudioState *state, const VencConfig *vcfg)
{
	MI_SYS_ChnPort_t ai_port;
	int ret;

	ret = state->lib.fnEnableDevice(0);
	if (ret != 0) {
		fprintf(stderr, "[audio] ERROR: EnableDevice failed (%d)\n", ret);
		return -1;
	}
	state->device_enabled = 1;

	memset(&ai_port, 0, sizeof(ai_port));
	ai_port.module = I6_SYS_MOD_AI;
	ai_port.device = 0;
	ai_port.channel = 0;
	ai_port.port = 0;
	/* user_depth=1, buf_depth=2: minimum port buffering (40ms at 20ms/frame).
	 * Previous values (2, 4) added another 80ms of latency on top of frmNum. */
	ret = MI_SYS_SetChnOutputPortDepth(&ai_port, 1, 2);
	if (ret != 0) {
		fprintf(stderr, "[audio] ERROR: SetChnOutputPortDepth failed (%d)\n", ret);
		return -1;
	}

	ret = state->lib.fnEnableChannel(0, 0);
	if (ret != 0) {
		fprintf(stderr, "[audio] ERROR: EnableChannel failed (%d)\n", ret);
		return -1;
	}
	state->channel_enabled = 1;

	state->lib.fnSetVolume(0, 0, star6e_audio_volume_to_db(vcfg->audio.volume));
	if (vcfg->audio.mute)
		state->lib.fnSetMute(0, 0, 1);
	return 0;
}

static int start_audio_output_and_thread(Star6eAudioState *state,
	const Star6eOutput *output, const VencConfig *vcfg)
{
	if (star6e_audio_output_init(&state->output, output,
	    vcfg->outgoing.audio_port, vcfg->outgoing.max_payload_size) != 0)
		return -1;

	if (star6e_output_is_rtp(output)) {
		state->rtp.seq = (uint16_t)(rand() & 0xFFFF);
		state->rtp.timestamp = (uint32_t)rand();
		state->rtp.ssrc = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ 0xA0D1DEAD;
		/* Use standard static PTs when rate matches RFC 3551,
		 * dynamic PTs for non-standard rates */
		if (state->codec_type == AUDIO_TYPE_G711U &&
		    state->sample_rate == 8000)
			state->rtp.payload_type = 0;   /* PCMU 8kHz mono */
		else if (state->codec_type == AUDIO_TYPE_G711A &&
		         state->sample_rate == 8000)
			state->rtp.payload_type = 8;   /* PCMA 8kHz mono */
		else if (state->codec_type == AUDIO_TYPE_G711U)
			state->rtp.payload_type = 112; /* PCMU non-8kHz */
		else if (state->codec_type == AUDIO_TYPE_G711A)
			state->rtp.payload_type = 113; /* PCMA non-8kHz */
		else if (state->codec_type == AUDIO_TYPE_OPUS)
			state->rtp.payload_type = 98; /* dynamic PT for Opus (majestic convention) */
		else if (state->codec_type < 0 &&
		         state->sample_rate == 44100)
			state->rtp.payload_type = 11;  /* L16 44.1kHz mono */
		else
			state->rtp.payload_type = 110; /* dynamic PCM */
		/* Opus uses a 48kHz nominal RTP clock per RFC 7587 §4.2.
		 * All other codecs use samples per frame at the capture rate. */
		if (state->codec_type == AUDIO_TYPE_OPUS)
			state->rtp_frame_ticks = OPUS_RTP_CLOCK_HZ / 50;
		else
			state->rtp_frame_ticks = (unsigned int)(state->sample_rate / 50);
	} else {
		memset(&state->rtp, 0, sizeof(state->rtp));
		state->rtp_frame_ticks = 0;
	}

	if (output->transport == VENC_OUTPUT_URI_UDP &&
	    star6e_audio_output_port(&state->output) == 0)
		fprintf(stderr, "[audio] WARNING: audio output has no destination port\n");

	audio_ring_init(&state->cap_ring);

	state->running = 1;
	if (pthread_create(&state->capture_thread, NULL,
	    star6e_audio_capture_fn, state) != 0) {
		fprintf(stderr, "[audio] ERROR: capture pthread_create failed\n");
		state->running = 0;
		audio_ring_destroy(&state->cap_ring);
		return -1;
	}
	/* SCHED_FIFO priority 1 (minimum RT) is sufficient: the MI AI kernel
	 * thread (ai0_P0_MAIN) signals frame-ready at SCHED_RR/98, and any
	 * briefly-delayed fetch is handled by the 400ms DMA ring (frmNum=20).
	 * Higher priorities (e.g. 90) were tested and made timing worse. */
	{
		struct sched_param sp;
		sp.sched_priority = 1;
		if (pthread_setschedparam(state->capture_thread, SCHED_FIFO,
		    &sp) != 0 && state->verbose)
			fprintf(stderr, "[audio] note: could not set RT priority"
				" (run as root?)\n");
	}

	if (pthread_create(&state->encode_thread, NULL,
	    star6e_audio_encode_fn, state) != 0) {
		fprintf(stderr, "[audio] ERROR: encode pthread_create failed\n");
		state->running = 0;
		audio_ring_wake(&state->cap_ring);
		pthread_join(state->capture_thread, NULL);
		audio_ring_destroy(&state->cap_ring);
		return -1;
	}

	state->started = 1;
	return 0;
}

int star6e_audio_init(Star6eAudioState *state, const VencConfig *vcfg,
	const Star6eOutput *output)
{
	if (!state)
		return 0;

	memset(state, 0, sizeof(*state));
	star6e_audio_output_reset(&state->output);
	if (!vcfg || !vcfg->audio.enabled || !output)
		return 0;

	state->sample_rate = vcfg->audio.sample_rate;
	state->channels = vcfg->audio.channels;
	state->verbose = vcfg->system.verbose;

	state->codec_type = -1;
	if (strcmp(vcfg->audio.codec, "g711a") == 0)
		state->codec_type = AUDIO_TYPE_G711A;
	else if (strcmp(vcfg->audio.codec, "g711u") == 0)
		state->codec_type = AUDIO_TYPE_G711U;
	else if (strcmp(vcfg->audio.codec, "opus") == 0)
		state->codec_type = AUDIO_TYPE_OPUS;
	else if (strcmp(vcfg->audio.codec, "pcm") != 0)
		fprintf(stderr, "[audio] WARNING: unknown codec '%s', using raw PCM\n",
			vcfg->audio.codec);

	if (state->codec_type == AUDIO_TYPE_OPUS) {
		if (audio_codec_opus_init(&state->opus, state->sample_rate,
		    state->channels) != 0)
			state->codec_type = -1;
	}

	/* Install stdout filter before touching the MI AI library.  The
	 * library's internal thread (ai0_P0_MAIN) writes "[MI WRN]" lines to
	 * stdout at any time once the device is enabled — including on reinit
	 * when the device stays enabled across the stop/start cycle. */
	audio_codec_stdout_filter_start();

	if (g_ai_persist.initialized) {
		/* Reinit: restore persisted lib and device state, only restart
		 * the output socket and capture thread. */
		state->lib = g_ai_persist.lib;
		state->lib_loaded = 1;
		state->device_enabled = 1;
		state->channel_enabled = 1;
		state->lib.fnSetVolume(0, 0,
			star6e_audio_volume_to_db(vcfg->audio.volume));
		if (vcfg->audio.mute)
			state->lib.fnSetMute(0, 0, 1);
	} else {
		if (star6e_audio_lib_load(&state->lib) != 0) {
			fprintf(stderr, "[audio] WARNING: audio enabled but libmi_ai.so not available\n");
			audio_codec_stdout_filter_stop();
			return 0;
		}
		state->lib_loaded = 1;

		if (configure_ai_device(state) != 0)
			goto fail;
		if (start_ai_capture(state, vcfg) != 0)
			goto fail;

		g_ai_persist.lib = state->lib;
		g_ai_persist.initialized = 1;
	}

	if (start_audio_output_and_thread(state, output, vcfg) != 0)
		goto fail;

	if (state->verbose) {
		uint16_t port = star6e_audio_output_port(&state->output);
		if (port != 0) {
			printf("[audio] Initialized: %s @ %u Hz, %u ch, port %u\n",
				vcfg->audio.codec, state->sample_rate, state->channels,
				port);
		} else {
			printf("[audio] Initialized: %s @ %u Hz, %u ch, shared output transport\n",
				vcfg->audio.codec, state->sample_rate, state->channels);
		}
	}
	return 0;

fail:
	star6e_audio_teardown_opus(state);
	if (!g_ai_persist.initialized) {
		if (state->channel_enabled) {
			state->lib.fnDisableChannel(0, 0);
			state->channel_enabled = 0;
		}
		if (state->device_enabled) {
			state->lib.fnDisableDevice(0);
			state->device_enabled = 0;
		}
		star6e_audio_lib_unload(&state->lib);
		state->lib_loaded = 0;
	}
	star6e_audio_output_teardown(&state->output);
	audio_codec_stdout_filter_stop();
	fprintf(stderr, "[audio] WARNING: audio init failed, continuing without audio\n");
	return 0;
}

static void star6e_audio_teardown_opus(Star6eAudioState *state)
{
	audio_codec_opus_teardown(&state->opus);
}

void star6e_audio_teardown(Star6eAudioState *state)
{
	if (!state)
		return;

	/* Skip AI device teardown if previously initialized.  Cycling
	 * MI_AI_Disable/Enable on a kernel-tracked AI device deadlocks
	 * CamOsMutexLock on Star6E — observed during pipeline_stop with
	 * audio thread joined cleanly but the channel/device disable below
	 * hanging, and the watchdog SIGKILL'ing us before MI_SYS_Exit could
	 * run.  In-process reinit no longer rebuilds the pipeline (we
	 * fork+exec for that — see star6e_runtime_respawn_after_exit),
	 * so the only consumer of this teardown is the final shutdown,
	 * where kernel-side cleanup runs on process exit anyway. */

	if (state->started) {
		state->running = 0;
		/* Wake encode thread in case it is blocked in pop_wait */
		audio_ring_wake(&state->cap_ring);
		pthread_join(state->capture_thread, NULL);
		pthread_join(state->encode_thread, NULL);
		if (state->cap_ring.dropped)
			fprintf(stderr, "[audio] cap_ring dropped %u frames\n",
				state->cap_ring.dropped);
		audio_ring_destroy(&state->cap_ring);
		state->started = 0;
	}
	star6e_audio_teardown_opus(state);
	if (state->lib_loaded && !g_ai_persist.initialized) {
		if (state->channel_enabled) {
			state->lib.fnDisableChannel(0, 0);
			state->channel_enabled = 0;
		}
		if (state->device_enabled) {
			state->lib.fnDisableDevice(0);
			state->device_enabled = 0;
		}
		star6e_audio_lib_unload(&state->lib);
		state->lib_loaded = 0;
	}
	star6e_audio_output_teardown(&state->output);
	audio_codec_stdout_filter_stop();
}

int star6e_audio_apply_mute(Star6eAudioState *state, int muted)
{
	int ret;

	if (!state || !state->lib_loaded || !state->channel_enabled ||
	    !state->lib.fnSetMute) {
		return -1;
	}

	ret = state->lib.fnSetMute(0, 0, muted ? 1 : 0);
	if (ret != 0) {
		fprintf(stderr, "[audio] SetMute(%d) failed: %d\n", muted ? 1 : 0, ret);
		return -1;
	}

	return 0;
}
