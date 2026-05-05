#include "ts_mux.h"

#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static int test_ts_mux_init(void)
{
	TsMuxState s;
	int failures = 0;

	ts_mux_init(&s, 16000, 1, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("ts_mux init cc_pat", s.cc_pat == 0);
	CHECK("ts_mux init cc_pmt", s.cc_pmt == 0);
	CHECK("ts_mux init cc_video", s.cc_video == 0);
	CHECK("ts_mux init cc_audio", s.cc_audio == 0);
	CHECK("ts_mux init audio_rate", s.audio_rate == 16000);
	CHECK("ts_mux init audio_channels", s.audio_channels == 1);
	CHECK("ts_mux init video_frames forces pat/pmt",
		s.video_frames >= TS_PAT_PMT_INTERVAL);

	ts_mux_init(NULL, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("ts_mux init null no crash", 1);
	return failures;
}

static int test_ts_mux_pat_pmt(void)
{
	TsMuxState s;
	uint8_t buf[2 * TS_PACKET_SIZE];
	size_t written;
	int failures = 0;

	ts_mux_init(&s, 16000, 1, TS_AUDIO_CODEC_PCM_S302M);
	written = ts_mux_write_pat_pmt(&s, buf, sizeof(buf));
	CHECK("pat_pmt writes 2 packets", written == 2 * TS_PACKET_SIZE);

	/* PAT sync byte and PID */
	CHECK("pat sync byte", buf[0] == TS_SYNC_BYTE);
	CHECK("pat pid high", (buf[1] & 0x1F) == 0x00);
	CHECK("pat pid low", buf[2] == 0x00);
	CHECK("pat pusi set", (buf[1] & 0x40) != 0);

	/* PMT sync byte and PID */
	uint8_t *pmt = buf + TS_PACKET_SIZE;
	CHECK("pmt sync byte", pmt[0] == TS_SYNC_BYTE);
	uint16_t pmt_pid = (uint16_t)(((pmt[1] & 0x1F) << 8) | pmt[2]);
	CHECK("pmt pid", pmt_pid == TS_PID_PMT);
	CHECK("pmt pusi set", (pmt[1] & 0x40) != 0);

	/* CC increments */
	CHECK("pat cc after", s.cc_pat == 1);
	CHECK("pmt cc after", s.cc_pmt == 1);
	return failures;
}

static int test_ts_mux_pat_pmt_no_audio(void)
{
	TsMuxState s;
	uint8_t buf[2 * TS_PACKET_SIZE];
	size_t written;
	int failures = 0;

	ts_mux_init(&s, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	written = ts_mux_write_pat_pmt(&s, buf, sizeof(buf));
	CHECK("pat_pmt no audio writes 2 packets", written == 2 * TS_PACKET_SIZE);
	CHECK("pat_pmt no audio sync", buf[0] == TS_SYNC_BYTE);
	return failures;
}

static int test_ts_mux_pat_pmt_small_buf(void)
{
	TsMuxState s;
	uint8_t buf[100];
	int failures = 0;

	ts_mux_init(&s, 16000, 1, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("pat_pmt small buf returns 0",
		ts_mux_write_pat_pmt(&s, buf, sizeof(buf)) == 0);
	return failures;
}

static int test_ts_mux_write_video(void)
{
	TsMuxState s;
	/* Enough for PAT+PMT + several video packets */
	uint8_t buf[20 * TS_PACKET_SIZE];
	uint8_t data[500];
	size_t written;
	int failures = 0;

	memset(data, 0xAB, sizeof(data));
	ts_mux_init(&s, 16000, 1, TS_AUDIO_CODEC_PCM_S302M);

	written = ts_mux_write_video(&s, buf, sizeof(buf),
		data, sizeof(data), 90000, 1);

	CHECK("write_video non-zero", written > 0);
	CHECK("write_video multiple of 188", (written % TS_PACKET_SIZE) == 0);

	/* First two packets should be PAT+PMT (since video_frames was at interval) */
	CHECK("first packet is PAT sync", buf[0] == TS_SYNC_BYTE);
	uint16_t first_pid = (uint16_t)(((buf[1] & 0x1F) << 8) | buf[2]);
	CHECK("first packet is PAT", first_pid == TS_PID_PAT);

	/* Video packets follow */
	size_t pkt_count = written / TS_PACKET_SIZE;
	CHECK("at least 4 packets", pkt_count >= 4);

	/* Check all packets have sync byte */
	int all_sync = 1;
	for (size_t i = 0; i < pkt_count; i++) {
		if (buf[i * TS_PACKET_SIZE] != TS_SYNC_BYTE) {
			all_sync = 0;
			break;
		}
	}
	CHECK("all packets have sync byte", all_sync);

	/* Check video_frames was incremented */
	CHECK("video_frames incremented", s.video_frames == 1);
	return failures;
}

static int test_ts_mux_write_video_no_patpmt(void)
{
	TsMuxState s;
	uint8_t buf[20 * TS_PACKET_SIZE];
	uint8_t data[100];
	size_t written;
	int failures = 0;

	memset(data, 0xCD, sizeof(data));
	ts_mux_init(&s, 0, 0, TS_AUDIO_CODEC_PCM_S302M);

	/* Write first frame — consumes PAT/PMT */
	ts_mux_write_video(&s, buf, sizeof(buf), data, sizeof(data), 90000, 0);

	/* Second frame should NOT have PAT/PMT */
	written = ts_mux_write_video(&s, buf, sizeof(buf),
		data, sizeof(data), 180000, 0);
	CHECK("no pat_pmt second frame", written > 0);

	/* First packet of second write should be video PID */
	uint16_t pid = (uint16_t)(((buf[1] & 0x1F) << 8) | buf[2]);
	CHECK("second frame starts with video pid", pid == TS_PID_VIDEO);
	return failures;
}

static int test_ts_mux_write_audio(void)
{
	TsMuxState s;
	uint8_t buf[40 * TS_PACKET_SIZE];
	/* 960 mono s16le samples = 20 ms @ 48 kHz */
	uint8_t data[1920];
	size_t written;
	int failures = 0;

	memset(data, 0x55, sizeof(data));
	ts_mux_init(&s, 48000, 1, TS_AUDIO_CODEC_PCM_S302M);

	written = ts_mux_write_audio(&s, buf, sizeof(buf),
		data, sizeof(data), 45000);

	CHECK("write_audio non-zero", written > 0);
	CHECK("write_audio multiple of 188", (written % TS_PACKET_SIZE) == 0);

	/* Check PID is audio */
	uint16_t pid = (uint16_t)(((buf[1] & 0x1F) << 8) | buf[2]);
	CHECK("audio packet pid", pid == TS_PID_AUDIO);
	CHECK("audio cc tracks packet count",
		s.cc_audio == ((written / TS_PACKET_SIZE) & 0x0F));

	/* Locate PES payload start: TS header (4) + PES header (14) = byte 18.
	 * First 4 bytes of PES payload are the SMPTE 302M audio_data_header. */
	const uint8_t *aes = buf + 4 + 14;
	/* 960 sample-frames × 5 bytes per stereo pair (mono upmixed) = 4800 */
	uint16_t aes_size = (uint16_t)((aes[0] << 8) | aes[1]);
	CHECK("aes audio_packet_size = 4800", aes_size == 4800);
	/* number_channels: stereo = 00 → top 2 bits of byte 2 are 0 */
	CHECK("aes number_channels stereo", (aes[2] & 0xC0) == 0x00);
	/* bits_per_sample: 16-bit = 00 → bits 5..4 of byte 3 are 0 */
	CHECK("aes bits_per_sample 16", (aes[3] & 0x30) == 0x00);
	return failures;
}

static int test_ts_mux_audio_bssd_descriptor(void)
{
	TsMuxState s;
	uint8_t buf[2 * TS_PACKET_SIZE];
	int failures = 0;

	ts_mux_init(&s, 48000, 1, TS_AUDIO_CODEC_PCM_S302M);
	ts_mux_write_pat_pmt(&s, buf, sizeof(buf));

	/* Search the PMT packet for the 4-byte "BSSD" registration tag. */
	const uint8_t *pmt = buf + TS_PACKET_SIZE;
	int found_bssd = 0;
	for (size_t i = 0; i + 3 < TS_PACKET_SIZE; i++) {
		if (pmt[i] == 'B' && pmt[i + 1] == 'S' &&
		    pmt[i + 2] == 'S' && pmt[i + 3] == 'D') {
			found_bssd = 1;
			break;
		}
	}
	CHECK("PMT carries BSSD registration descriptor", found_bssd);
	return failures;
}

static int test_ts_mux_audio_opus_pmt(void)
{
	TsMuxState s;
	uint8_t buf[2 * TS_PACKET_SIZE];
	int failures = 0;

	ts_mux_init(&s, 48000, 1, TS_AUDIO_CODEC_OPUS);
	ts_mux_write_pat_pmt(&s, buf, sizeof(buf));

	const uint8_t *pmt = buf + TS_PACKET_SIZE;
	int found_opus = 0, found_ext = 0;
	for (size_t i = 0; i + 3 < TS_PACKET_SIZE; i++) {
		if (pmt[i] == 'O' && pmt[i + 1] == 'p' &&
		    pmt[i + 2] == 'u' && pmt[i + 3] == 's')
			found_opus = 1;
		/* extension_descriptor 0x7F len=2 ext_tag=0x80 chcfg=1 (mono) */
		if (pmt[i] == 0x7F && pmt[i + 1] == 0x02 &&
		    pmt[i + 2] == 0x80 && pmt[i + 3] == 0x01)
			found_ext = 1;
	}
	CHECK("PMT carries Opus registration descriptor", found_opus);
	CHECK("PMT carries Opus extension descriptor", found_ext);
	return failures;
}

static int test_ts_mux_write_audio_opus(void)
{
	TsMuxState s;
	uint8_t buf[10 * TS_PACKET_SIZE];
	/* Fake Opus access unit — content doesn't matter for framing. */
	uint8_t opus[120];
	size_t written;
	int failures = 0;

	memset(opus, 0xAA, sizeof(opus));
	ts_mux_init(&s, 48000, 1, TS_AUDIO_CODEC_OPUS);

	written = ts_mux_write_audio(&s, buf, sizeof(buf),
		opus, sizeof(opus), 90000);

	CHECK("write_audio opus non-zero", written > 0);
	CHECK("write_audio opus aligned", (written % TS_PACKET_SIZE) == 0);

	/* PES payload starts at TS(4) + PES(14) = 18.
	 * First 2 bytes are the Opus control_header_prefix = 0x7F 0xE0;
	 * byte 2 = au_size = 120 (0x78). */
	const uint8_t *pes = buf + 4 + 14;
	CHECK("opus prefix byte 0 = 0x7F", pes[0] == 0x7F);
	CHECK("opus prefix byte 1 = 0xE0", pes[1] == 0xE0);
	CHECK("opus au_size = 120",         pes[2] == 0x78);
	CHECK("opus payload first byte",    pes[3] == 0xAA);
	return failures;
}

static int test_ts_mux_write_null_args(void)
{
	TsMuxState s;
	uint8_t buf[TS_PACKET_SIZE];
	uint8_t data[10];
	int failures = 0;

	ts_mux_init(&s, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("write_video null state",
		ts_mux_write_video(NULL, buf, sizeof(buf), data, 10, 0, 0) == 0);
	CHECK("write_video null buf",
		ts_mux_write_video(&s, NULL, 0, data, 10, 0, 0) == 0);
	CHECK("write_video null data",
		ts_mux_write_video(&s, buf, sizeof(buf), NULL, 10, 0, 0) == 0);
	CHECK("write_video zero len",
		ts_mux_write_video(&s, buf, sizeof(buf), data, 0, 0, 0) == 0);
	CHECK("write_audio null state",
		ts_mux_write_audio(NULL, buf, sizeof(buf), data, 10, 0) == 0);
	return failures;
}

static int test_ts_mux_timespec_to_pts(void)
{
	int failures = 0;

	/* 1 second = 90000 ticks */
	CHECK("1s = 90000", ts_mux_timespec_to_pts(1, 0) == 90000);
	/* 0.5s = 45000 ticks */
	CHECK("0.5s = 45000", ts_mux_timespec_to_pts(0, 500000000) == 45000);
	/* 0 = 0 */
	CHECK("0 = 0", ts_mux_timespec_to_pts(0, 0) == 0);
	return failures;
}

static int test_ts_mux_cc_wraps(void)
{
	TsMuxState s;
	uint8_t buf[20 * TS_PACKET_SIZE];
	uint8_t data[10];
	int failures = 0;

	memset(data, 0x01, sizeof(data));
	ts_mux_init(&s, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	s.video_frames = 0;  /* prevent PAT/PMT */

	/* Write 20 frames — CC should wrap at 16 */
	for (int i = 0; i < 20; i++) {
		ts_mux_write_video(&s, buf, sizeof(buf), data, sizeof(data),
			(uint64_t)i * 3000, 0);
	}
	/* CC should have wrapped */
	CHECK("cc wraps around 16", s.cc_video < 16);
	return failures;
}

static int test_ts_mux_small_data(void)
{
	TsMuxState s;
	uint8_t buf[10 * TS_PACKET_SIZE];
	uint8_t data[1] = { 0xFF };
	size_t written;
	int failures = 0;

	ts_mux_init(&s, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	s.video_frames = 0;

	written = ts_mux_write_video(&s, buf, sizeof(buf),
		data, sizeof(data), 90000, 0);
	CHECK("small data produces at least 1 packet",
		written >= TS_PACKET_SIZE);
	CHECK("small data aligned", (written % TS_PACKET_SIZE) == 0);
	return failures;
}

int test_ts_mux(void)
{
	int failures = 0;

	failures += test_ts_mux_init();
	failures += test_ts_mux_pat_pmt();
	failures += test_ts_mux_pat_pmt_no_audio();
	failures += test_ts_mux_pat_pmt_small_buf();
	failures += test_ts_mux_write_video();
	failures += test_ts_mux_write_video_no_patpmt();
	failures += test_ts_mux_write_audio();
	failures += test_ts_mux_audio_bssd_descriptor();
	failures += test_ts_mux_audio_opus_pmt();
	failures += test_ts_mux_write_audio_opus();
	failures += test_ts_mux_write_null_args();
	failures += test_ts_mux_timespec_to_pts();
	failures += test_ts_mux_cc_wraps();
	failures += test_ts_mux_small_data();

	return failures;
}
