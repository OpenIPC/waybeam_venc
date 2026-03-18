#ifndef STAR6E_AUDIO_H
#define STAR6E_AUDIO_H

#include "audio_ring.h"
#include "star6e_output.h"
#include "venc_config.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>

typedef struct Star6eAudioDevConfig Star6eAudioDevConfig;
typedef struct Star6eAudioFrame Star6eAudioFrame;

typedef struct {
	void *handle;
	int (*fnDisableDevice)(int device);
	int (*fnEnableDevice)(int device);
	int (*fnSetDeviceConfig)(int device, Star6eAudioDevConfig *config);
	int (*fnDisableChannel)(int device, int channel);
	int (*fnEnableChannel)(int device, int channel);
	int (*fnSetMute)(int device, int channel, char active);
	int (*fnSetVolume)(int device, int channel, int db_level);
	int (*fnFreeFrame)(int device, int channel, Star6eAudioFrame *frame,
		void *aec_frame);
	int (*fnGetFrame)(int device, int channel, Star6eAudioFrame *frame,
		void *aec_frame, int millis);
} Star6eAudioLib;

typedef struct {
	Star6eAudioLib lib;
	int lib_loaded;
	int device_enabled;
	int channel_enabled;
	int codec_type;
	uint32_t sample_rate;
	uint32_t channels;
	Star6eAudioOutput output;
	RtpPacketizerState rtp;
	uint32_t rtp_frame_ticks;
	pthread_t capture_thread;  /* Thread A: GetFrame→copy→release only */
	pthread_t encode_thread;   /* Thread B: encode→send */
	volatile sig_atomic_t running;
	volatile sig_atomic_t started;
	int verbose;
	AudioRing cap_ring;    /* capture→encode bridge (owned by this struct) */
	AudioRing *rec_ring;   /* recording ring buffer (NULL if not recording) */
	void *opus_lib;        /* dlopen handle for libopus.so (NULL if not Opus) */
	void *opus_enc;        /* OpusEncoder* opaque handle (NULL if not Opus) */
} Star6eAudioState;

/** Initialize audio capture, encoder, and RTP output thread. */
int star6e_audio_init(Star6eAudioState *state, const VencConfig *vcfg,
	const Star6eOutput *output);

/** Stop audio thread and release all audio resources. */
void star6e_audio_teardown(Star6eAudioState *state);

/** Apply mute/unmute to audio encoder channel. */
int star6e_audio_apply_mute(Star6eAudioState *state, int muted);

#endif /* STAR6E_AUDIO_H */
