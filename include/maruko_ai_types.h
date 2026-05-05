#ifndef MARUKO_AI_TYPES_H
#define MARUKO_AI_TYPES_H

/*
 * Maruko (Infinity6C) MI_AI ABI types.
 *
 * Copied verbatim from the SigmaStar SDK headers
 * `mi_ai_datatype.h` / `mi_aio_datatype.h` so the build does not depend on
 * vendor SDK headers.  Layout MUST match the binary `libmi_ai.so` we ship in
 * `vendor-libs/maruko/` — keep these in sync if a SDK refresh changes them.
 *
 * Naming convention here uses the SigmaStar enum/struct names directly so a
 * future engineer can cross-reference SDK docs without translation.
 */

#include <stdint.h>

#define MI_AI_MAX_CHN_NUM         (8)
#define MI_AI_ECHO_CHN_GROUP_ID   (0xFF)

typedef enum {
	E_MI_AUDIO_FORMAT_INVALID    = -1,
	E_MI_AUDIO_FORMAT_PCM_S16_LE = 0,
} MARUKO_AI_Format_e;

typedef enum {
	E_MI_AUDIO_SAMPLE_RATE_8000   = 8000,
	E_MI_AUDIO_SAMPLE_RATE_16000  = 16000,
	E_MI_AUDIO_SAMPLE_RATE_32000  = 32000,
	E_MI_AUDIO_SAMPLE_RATE_48000  = 48000,
} MARUKO_AI_SampleRate_e;

typedef enum {
	E_MI_AUDIO_SOUND_MODE_MONO   = 1,
	E_MI_AUDIO_SOUND_MODE_STEREO = 2,
} MARUKO_AI_SoundMode_e;

typedef enum {
	MARUKO_AI_IF_NONE      = 0,
	MARUKO_AI_IF_ADC_AB    = 1,  /* Analog mic A/B */
	MARUKO_AI_IF_ADC_CD    = 2,  /* Analog mic C/D */
	MARUKO_AI_IF_DMIC_A_01 = 3,  /* Digital mic A 0/1 */
	MARUKO_AI_IF_DMIC_A_23 = 4,
	MARUKO_AI_IF_I2S_A_01  = 5,
	MARUKO_AI_IF_ECHO_A    = 37,
} MARUKO_AI_If_e;

/* MI_AI_Attr_t — passed to MI_AI_Open(devId, *attr).
 * Wire layout: 5 × 32-bit words (enum + period_size + bool padded). */
typedef struct {
	int32_t  enFormat;       /* MARUKO_AI_Format_e */
	int32_t  enSoundMode;    /* MARUKO_AI_SoundMode_e */
	int32_t  enSampleRate;   /* MARUKO_AI_SampleRate_e (uses literal Hz) */
	uint32_t u32PeriodSize;  /* frames per period (e.g. 1024) */
	int32_t  bInterleaved;   /* MI_BOOL */
} MARUKO_AI_Attr_t;

/* MI_AI_Data_t — populated by MI_AI_Read(); released by MI_AI_ReleaseData().
 * apvBuffer[0..MI_AI_MAX_CHN_NUM-1] are per-channel-group data pointers.
 * When bInterleaved=TRUE, only apvBuffer[0] / u32Byte[0] are populated. */
typedef struct {
	void     *apvBuffer[MI_AI_MAX_CHN_NUM];
	uint32_t  u32Byte[MI_AI_MAX_CHN_NUM];
	uint64_t  u64Pts;
	uint64_t  u64Seq;
} MARUKO_AI_Data_t;

#endif /* MARUKO_AI_TYPES_H */
