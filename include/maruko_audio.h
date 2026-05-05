#ifndef MARUKO_AUDIO_H
#define MARUKO_AUDIO_H

/*
 * Maruko (Infinity6C) audio capture + RTP/UDP output.
 *
 * Mirrors the Star6E shape (`star6e_audio.{c,h}`) but talks to the i6c
 * `MI_AI_Open / AttachIf / EnableChnGroup / Read / ReleaseData` API and
 * sends through MarukoOutput.  The encode pipeline (Opus / G.711 / raw
 * PCM) is shared via `audio_codec.h`.
 */

#include "audio_codec.h"
#include "audio_ring.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "rtp_packetizer.h"
#include "venc_config.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct {
	int             socket_handle;       /* -1 when uninitialised */
	uint16_t        port_override;       /* 0 → share video target */
	uint16_t        max_payload_size;
	struct sockaddr_storage fallback_dst;
	socklen_t       fallback_dst_len;
	const MarukoOutput *video_output;    /* borrowed; never freed here */
	struct sockaddr_storage cached_dst;
	socklen_t       cached_dst_len;
	int             cached_socket;
	uint32_t        cached_gen;
	int             cache_valid;
} MarukoAudioOutput;

typedef struct {
	int             lib_loaded;
	int             device_opened;
	int             group_enabled;
	int             codec_type;          /* AUDIO_CODEC_TYPE_* */
	uint32_t        sample_rate;
	uint32_t        channels;
	uint32_t        period_size;         /* frames per AI_Read pull */
	int             interface;           /* MARUKO_AI_IF_* */
	uint8_t         chn_grp;             /* always 0 on this hw */
	int             ai_dev;              /* always 0 on this hw */
	MarukoAudioOutput output;
	RtpPacketizerState rtp;
	uint32_t        rtp_frame_ticks;
	pthread_t       capture_thread;
	pthread_t       encode_thread;
	volatile sig_atomic_t running;
	volatile sig_atomic_t started;
	int             verbose;
	AudioRing       cap_ring;            /* owned bridge cap → encode */
	AudioRing      *rec_ring;            /* TS recorder (NULL when off) */
	AudioCodecOpus  opus;
} MarukoAudioState;

/** Initialize audio capture, encoder and RTP/UDP output thread.
 *  Returns 0 on success or when audio is disabled by config; never returns
 *  non-zero — failure is non-fatal (warning + continue without audio). */
int maruko_audio_init(MarukoAudioState *state, const MarukoBackendConfig *cfg,
	const MarukoOutput *output);

/** Stop threads and release all audio resources.  Safe to call when init
 *  was skipped (state->started == 0). */
void maruko_audio_teardown(MarukoAudioState *state);

/** Apply mute / unmute via MI_AI_SetMute. */
int maruko_audio_apply_mute(MarukoAudioState *state, int muted);

/** Build a JSON status snapshot describing whether the audio lib loaded,
 *  whether capture is running, the codec/rate/channels, and whether Opus
 *  was successfully initialized.  Returns a malloc'd NUL-terminated string
 *  the caller must free, or NULL on allocation failure. */
char *maruko_audio_query_status(const MarukoAudioState *state);

#endif /* MARUKO_AUDIO_H */
