#include "star6e_audio.h"

#include "star6e.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	uint32_t _pad0[4];
	uint16_t eModule;
	uint16_t _pad1;
	uint32_t u32Devid;
	uint32_t u32PrivateHeapSize;
} AudioDevPoolConf;

typedef struct {
	uint32_t _pad0[4];
	uint16_t eModule;
	uint16_t _pad1;
	uint32_t u32Devid;
	uint32_t u32Channel;
	uint32_t u32Port;
	uint32_t u32PrivateHeapSize;
} AudioChnPortPoolConf;

typedef struct {
	uint32_t eConfigType;
	char bCreate;
	char _pad[3];
	union {
		uint8_t _pad[64];
		AudioDevPoolConf stPreDevPrivPoolConfig;
		AudioChnPortPoolConf stPreChnPortOutputPrivPool;
	} uConfig;
} AudioMMAPoolConfig;

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

enum { AUDIO_TYPE_G711A = 0, AUDIO_TYPE_G711U = 1 };

extern int MI_SYS_ConfigPrivateMMAPool(AudioMMAPoolConfig *pstConf);

/* Persists AI device state across reinit cycles.  After removing MI_SYS_Exit
 * from the reinit path, the kernel AI device/channel state survives pipeline
 * stop/start.  Re-initializing it triggers a CamOsMutexLock deadlock after
 * 2+ VPE create/destroy cycles. */
static struct {
	int initialized;
	Star6eAudioLib lib;
} g_ai_persist;

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

static int star6e_audio_clock_for_rate(int rate)
{
	(void)rate;
	return 1;
}

static int star6e_audio_volume_to_db(int volume)
{
	if (volume <= 0)
		return -60;
	if (volume >= 100)
		return 30;
	return -60 + (volume * 90) / 100;
}

static uint8_t pcm16_to_alaw(int16_t pcm_in)
{
	int pcm = pcm_in;
	int sign = 0;
	int exponent;
	int mantissa;

	if (pcm < 0) {
		pcm = -pcm - 1;
		sign = 0x80;
	}
	if (pcm > 32635)
		pcm = 32635;

	if (pcm >= 256) {
		exponent = 7;
		for (int exp_mask = 0x4000; !(pcm & exp_mask) && exponent > 1;
		     exponent--, exp_mask >>= 1) {}
		mantissa = (pcm >> (exponent + 3)) & 0x0F;
	} else {
		exponent = 0;
		mantissa = pcm >> 4;
	}

	return (uint8_t)((sign | (exponent << 4) | mantissa) ^ 0xD5);
}

static uint8_t pcm16_to_ulaw(int16_t pcm_in)
{
	int pcm = pcm_in;
	int sign = 0;
	int exponent = 7;
	int mantissa;

	if (pcm < 0) {
		pcm = -pcm;
		sign = 0x80;
	}
	pcm += 132;
	if (pcm > 32767)
		pcm = 32767;

	for (int exp_mask = 0x4000; !(pcm & exp_mask) && exponent > 0;
	     exponent--, exp_mask >>= 1) {}
	mantissa = (pcm >> (exponent + 3)) & 0x0F;
	return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

static size_t star6e_audio_encode_g711(const int16_t *pcm, size_t num_samples,
	uint8_t *out, int codec_type)
{
	for (size_t i = 0; i < num_samples; i++) {
		out[i] = (codec_type == AUDIO_TYPE_G711A)
			? pcm16_to_alaw(pcm[i]) : pcm16_to_ulaw(pcm[i]);
	}
	return num_samples;
}

static void *star6e_audio_thread_fn(void *arg)
{
	Star6eAudioState *state = arg;
	Star6eAudioFrame frame;
	uint8_t enc_buf[2048];

	if (state->verbose)
		printf("[audio] Thread started (rate=%u, ch=%u, codec=%d%s)\n",
			state->sample_rate, state->channels, state->codec_type,
			state->codec_type != -1 ? " (sw encode)" : " (pcm)");

	while (state->running) {
		const uint8_t *data;
		size_t len;
		int ret;

		memset(&frame, 0, sizeof(frame));
		ret = state->lib.fnGetFrame(0, 0, &frame, NULL, 100);
		if (ret != 0)
			continue;

		data = frame.addr[0];
		len = frame.length;

		/* Push raw PCM to recording ring before codec encode */
		if (data && len > 0 && state->rec_ring) {
			struct timespec _ts;
			clock_gettime(CLOCK_MONOTONIC, &_ts);
			uint64_t ts_us = (uint64_t)_ts.tv_sec * 1000000ULL +
				(uint64_t)_ts.tv_nsec / 1000ULL;
			audio_ring_push(state->rec_ring, data,
				(uint16_t)(len > 0xFFFF ? 0xFFFF : len), ts_us);
		}

		if (data && len > 0 && state->codec_type >= 0) {
			size_t num_samples = len / 2;

			if (num_samples > sizeof(enc_buf))
				num_samples = sizeof(enc_buf);
			len = star6e_audio_encode_g711((const int16_t *)data, num_samples,
				enc_buf, state->codec_type);
			data = enc_buf;
		}

		if (data && len > 0) {
			(void)star6e_audio_output_send(&state->output, data, len,
				&state->rtp, state->rtp_frame_ticks);
		}

		state->lib.fnFreeFrame(0, 0, &frame, NULL);
	}

	if (state->verbose)
		printf("[audio] Thread stopped\n");
	return NULL;
}

static int configure_ai_device(Star6eAudioState *state)
{
	Star6eAudioDevConfig dev_cfg;
	AudioMMAPoolConfig pool_cfg;
	uint32_t dev_heap = 64 * 1024;
	uint32_t buf_size;
	uint32_t chn_heap;
	int pool_ret;
	int ret;

	memset(&dev_cfg, 0, sizeof(dev_cfg));
	dev_cfg.rate = (int)state->sample_rate;
	dev_cfg.intf = 0;
	dev_cfg.sound = (state->channels >= 2) ? 1 : 0;
	dev_cfg.frmNum = 16;
	dev_cfg.packNumPerFrm = 320;
	dev_cfg.codecChnNum = 0;
	dev_cfg.chnNum = state->channels;
	dev_cfg.i2s.clock = star6e_audio_clock_for_rate(dev_cfg.rate);

	buf_size = dev_cfg.packNumPerFrm * 2 * state->channels * 2;
	buf_size = ((buf_size + 4095) / 4096) * 4096;
	chn_heap = buf_size * dev_cfg.frmNum * 2;

	memset(&pool_cfg, 0, sizeof(pool_cfg));
	pool_cfg.eConfigType = 2;
	pool_cfg.bCreate = 1;
	pool_cfg.uConfig.stPreDevPrivPoolConfig.eModule = 4;
	pool_cfg.uConfig.stPreDevPrivPoolConfig.u32Devid = 0;
	pool_cfg.uConfig.stPreDevPrivPoolConfig.u32PrivateHeapSize = dev_heap;
	pool_ret = MI_SYS_ConfigPrivateMMAPool(&pool_cfg);
	if (pool_ret != 0)
		fprintf(stderr, "[audio] WARNING: ConfigPrivateMMAPool(dev) failed %d\n",
			pool_ret);

	memset(&pool_cfg, 0, sizeof(pool_cfg));
	pool_cfg.eConfigType = 3;
	pool_cfg.bCreate = 1;
	pool_cfg.uConfig.stPreChnPortOutputPrivPool.eModule = 4;
	pool_cfg.uConfig.stPreChnPortOutputPrivPool.u32Devid = 0;
	pool_cfg.uConfig.stPreChnPortOutputPrivPool.u32Channel = 0;
	pool_cfg.uConfig.stPreChnPortOutputPrivPool.u32Port = 0;
	pool_cfg.uConfig.stPreChnPortOutputPrivPool.u32PrivateHeapSize = chn_heap;
	pool_ret = MI_SYS_ConfigPrivateMMAPool(&pool_cfg);
	if (pool_ret != 0)
		fprintf(stderr, "[audio] WARNING: ConfigPrivateMMAPool(chn) failed %d\n",
			pool_ret);

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
	ret = MI_SYS_SetChnOutputPortDepth(&ai_port, 4, 16);
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
		/* Use standard static PTs where possible */
		if (state->codec_type == AUDIO_TYPE_G711U)
			state->rtp.payload_type = 0;   /* PCMU 8kHz */
		else if (state->codec_type == AUDIO_TYPE_G711A)
			state->rtp.payload_type = 8;   /* PCMA 8kHz */
		else if (state->sample_rate == 44100)
			state->rtp.payload_type = 11;  /* L16 44.1kHz mono */
		else
			state->rtp.payload_type = 110; /* dynamic (non-standard rate) */
		state->rtp_frame_ticks = 320;
	} else {
		memset(&state->rtp, 0, sizeof(state->rtp));
		state->rtp_frame_ticks = 0;
	}

	if (star6e_audio_output_port(&state->output) == 0)
		fprintf(stderr, "[audio] WARNING: audio output has no destination port\n");

	state->running = 1;
	if (pthread_create(&state->thread, NULL, star6e_audio_thread_fn, state) != 0) {
		fprintf(stderr, "[audio] ERROR: pthread_create failed\n");
		state->running = 0;
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
	else if (strcmp(vcfg->audio.codec, "pcm") != 0)
		fprintf(stderr, "[audio] WARNING: unknown codec '%s', using raw PCM\n",
			vcfg->audio.codec);

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

	if (state->verbose)
		printf("[audio] Initialized: %s @ %u Hz, %u ch, port %u\n",
			vcfg->audio.codec, state->sample_rate, state->channels,
			star6e_audio_output_port(&state->output));
	return 0;

fail:
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
	fprintf(stderr, "[audio] WARNING: audio init failed, continuing without audio\n");
	return 0;
}

void star6e_audio_teardown(Star6eAudioState *state)
{
	if (!state)
		return;

	if (state->started) {
		state->running = 0;
		pthread_join(state->thread, NULL);
		state->started = 0;
	}
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
