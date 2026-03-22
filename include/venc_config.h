#ifndef VENC_CONFIG_H
#define VENC_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VENC_CONFIG_DEFAULT_PATH "/etc/venc.json"
#define VENC_CONFIG_STRING_MAX 256

/* ── Sub-structs mirroring JSON sections ─────────────────────────────── */

typedef struct {
	uint16_t web_port;
	int overclock_level;   /* 0..2 */
	bool verbose;
} VencConfigSystem;

typedef struct {
	int index;             /* -1 = auto */
	int mode;              /* -1 = auto */
	bool unlock_enabled;
	uint32_t unlock_cmd;
	uint16_t unlock_reg;
	uint16_t unlock_value;
	int unlock_dir;        /* 0 = to_driver, 1 = to_user */
} VencConfigSensor;

typedef struct {
	char sensor_bin[VENC_CONFIG_STRING_MAX];
	uint32_t exposure;     /* milliseconds in JSON config, 0 = auto */
	bool legacy_ae;        /* true = use legacy ISP AE + handoff instead of custom AE */
	uint32_t ae_fps;       /* custom AE rate in Hz (default 15) */
	uint32_t gain_max;     /* max sensor gain (0 = use ISP bin default) */
	char awb_mode[16];     /* "auto" or "ct_manual" */
	uint32_t awb_ct;       /* color temperature in Kelvin (for ct_manual) */
} VencConfigIsp;

typedef struct {
	bool mirror;
	bool flip;
	int rotate;            /* 0 or 180 */
} VencConfigImage;

typedef struct {
	char codec[16];        /* "h264" or "h265" */
	char rc_mode[16];      /* "cbr", "vbr", "avbr", "qvbr" */
	uint32_t fps;
	uint32_t width;
	uint32_t height;
	uint32_t bitrate;      /* kbps */
	double gop_size;       /* seconds between keyframes; 0 = all-intra */
	int qp_delta;          /* relative I/P QP delta, -12..12 */
	bool frame_lost;       /* enable frame-lost safety net */
} VencConfigVideo;

typedef struct {
	bool enabled;
	uint32_t sample_rate;   /* Hz: 8000, 16000, 48000 */
	uint32_t channels;      /* 1 or 2 */
	char codec[16];         /* "pcm", "g711a", "g711u", "opus" */
	int volume;             /* 0..100 */
	bool mute;
} VencConfigAudio;

typedef struct {
	bool enabled;
	char server[VENC_CONFIG_STRING_MAX]; /* "udp://host:port" */
	char stream_mode[16];               /* "rtp" or "compact" */
	uint16_t max_payload_size;
	bool connected_udp;             /* connect() socket (skip per-packet routing) */
	uint16_t audio_port;                /* 0 = same as video port — AVOID: mixing
	                                     * audio and video RTP on one socket causes
	                                     * video decoder instability at the receiver */
	uint16_t sidecar_port;              /* 0 = disabled */
} VencConfigOutgoing;

typedef struct {
	bool roi_enabled;
	int roi_qp;            /* signed ROI delta QP, -30..30 */
	uint16_t roi_steps;    /* 1..4 horizontal band regions */
	double roi_center;     /* center region fraction 0.1..0.9 */
	int noise_level;       /* 0..7 */
} VencConfigFpv;

typedef struct {
	bool enabled;
	char i2c_device[VENC_CONFIG_STRING_MAX];  /* "/dev/i2c-1" */
	uint8_t i2c_addr;                         /* 0x68 */
	int sample_rate_hz;                        /* 200 */
	int gyro_range_dps;                        /* 1000 */
	char cal_file[VENC_CONFIG_STRING_MAX];     /* "/etc/imu.cal" */
	int cal_samples;                           /* 400 */
} VencConfigImu;

typedef struct {
	bool enabled;
	char mode[16];            /* "gyroglide" (default) */
	int margin_percent;       /* 30 */
	bool test_mode;
	bool swap_xy;
	bool invert_x;
	bool invert_y;
	/* GyroGlide-specific (ignored by legacy backend) */
	float gain;               /* 0.8 — correction gain */
	float deadband_rad;       /* 0.001 — per-frame angle threshold */
	float recenter_rate;      /* 1.0 — return-to-center speed (1/s) */
	float max_slew_px;        /* 8.0 — max crop change per frame */
	float bias_alpha;         /* 0.001 — runtime bias adaptation rate */
} VencConfigEis;

typedef struct {
	bool enabled;
	char dir[VENC_CONFIG_STRING_MAX];
	char format[16];          /* "hevc" or "ts", default "ts" */
	char mode[16];            /* "off","mirror","dual","dual-stream" */
	uint32_t max_seconds;     /* rotation interval: 0=off, default 300 */
	uint32_t max_mb;          /* rotation size in MB: 0=off, default 500 */
	/* Dual/gemini channel settings (used when mode=dual or dual-stream) */
	uint32_t bitrate;         /* ch1 bitrate kbps, 0=match ch0 */
	uint32_t fps;             /* ch1 fps, 0=match sensor */
	double gop_size;          /* ch1 GOP in seconds, 0=match ch0 */
	char server[VENC_CONFIG_STRING_MAX]; /* dual-stream destination URI */
} VencConfigRecord;

/* ── Top-level config ────────────────────────────────────────────────── */

typedef struct {
	VencConfigSystem system;
	VencConfigSensor sensor;
	VencConfigIsp isp;
	VencConfigImage image;
	VencConfigVideo video0;
	VencConfigOutgoing outgoing;
	VencConfigFpv fpv;
	VencConfigAudio audio;
	VencConfigImu imu;
	VencConfigEis eis;
	VencConfigRecord record;
} VencConfig;

/* Fill cfg with compiled defaults. */
void venc_config_defaults(VencConfig *cfg);

/* Load JSON config from path into cfg.  Missing keys keep their current
 * (default) values.  Returns 0 on success, -1 on parse error.
 * If the file does not exist, returns 0 (defaults are used). */
int venc_config_load(const char *path, VencConfig *cfg);

/* Parse the outgoing.server URI ("udp://host:port") into separate host and
 * port values.  Returns 0 on success, -1 on parse error. */
int venc_config_parse_server_uri(const char *uri, char *host, size_t host_len,
	uint16_t *port);

/* Serialize config to a newly allocated JSON string.  Caller must free(). */
char *venc_config_to_json_string(const VencConfig *cfg);

/* Save current config to a JSON file at path.
 * Returns 0 on success, -1 on write error. */
int venc_config_save(const char *path, const VencConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* VENC_CONFIG_H */
