#include "ts_mux.h"

#include <string.h>

/* ── CRC32 for MPEG-TS (polynomial 0x04C11DB7, table-based) ─────────── */

static const uint32_t ts_crc32_table[256] = {
	0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9,
	0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
	0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61,
	0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
	0x4C11DB70, 0x48D0C6C7, 0x4593E01E, 0x4152FDA9,
	0x5F15ADAC, 0x5BD4B01B, 0x569796C2, 0x52568B75,
	0x6A1936C8, 0x6ED82B7F, 0x639B0DA6, 0x675A1011,
	0x791D4014, 0x7DDC5DA3, 0x709F7B7A, 0x745E66CD,
	0x9823B6E0, 0x9CE2AB57, 0x91A18D8E, 0x95609039,
	0x8B27C03C, 0x8FE6DD8B, 0x82A5FB52, 0x8664E6E5,
	0xBE2B5B58, 0xBAEA46EF, 0xB7A96036, 0xB3687D81,
	0xAD2F2D84, 0xA9EE3033, 0xA4AD16EA, 0xA06C0B5D,
	0xD4326D90, 0xD0F37027, 0xDDB056FE, 0xD9714B49,
	0xC7361B4C, 0xC3F706FB, 0xCEB42022, 0xCA753D95,
	0xF23A8028, 0xF6FB9D9F, 0xFBB8BB46, 0xFF79A6F1,
	0xE13EF6F4, 0xE5FFEB43, 0xE8BCCD9A, 0xEC7DD02D,
	0x34867077, 0x30476DC0, 0x3D044B19, 0x39C556AE,
	0x278206AB, 0x23431B1C, 0x2E003DC5, 0x2AC12072,
	0x128E9DCF, 0x164F8078, 0x1B0CA6A1, 0x1FCDBB16,
	0x018AEB13, 0x054BF6A4, 0x0808D07D, 0x0CC9CDCA,
	0x7897AB07, 0x7C56B6B0, 0x71159069, 0x75D48DDE,
	0x6B93DDDB, 0x6F52C06C, 0x6211E6B5, 0x66D0FB02,
	0x5E9F46BF, 0x5A5E5B08, 0x571D7DD1, 0x53DC6066,
	0x4D9B3063, 0x495A2DD4, 0x44190B0D, 0x40D816BA,
	0xACA5C697, 0xA864DB20, 0xA527FDF9, 0xA1E6E04E,
	0xBFA1B04B, 0xBB60ADFC, 0xB6238B25, 0xB2E29692,
	0x8AAD2B2F, 0x8E6C3698, 0x832F1041, 0x87EE0DF6,
	0x99A95DF3, 0x9D684044, 0x902B669D, 0x94EA7B2A,
	0xE0B41DE7, 0xE4750050, 0xE9362689, 0xEDF73B3E,
	0xF3B06B3B, 0xF771768C, 0xFA325055, 0xFEF34DE2,
	0xC6BCF05F, 0xC27DEDE8, 0xCF3ECB31, 0xCBFFD686,
	0xD5B88683, 0xD1799B34, 0xDC3ABDED, 0xD8FBA05A,
	0x690CE0EE, 0x6DCDFD59, 0x608EDB80, 0x644FC637,
	0x7A089632, 0x7EC98B85, 0x738AAD5C, 0x774BB0EB,
	0x4F040D56, 0x4BC510E1, 0x46863638, 0x42472B8F,
	0x5C007B8A, 0x58C1663D, 0x558240E4, 0x51435D53,
	0x251D3B9E, 0x21DC2629, 0x2C9F00F0, 0x285E1D47,
	0x36194D42, 0x32D850F5, 0x3F9B762C, 0x3B5A6B9B,
	0x0315D626, 0x07D4CB91, 0x0A97ED48, 0x0E56F0FF,
	0x1011A0FA, 0x14D0BD4D, 0x19939B94, 0x1D528623,
	0xF12F560E, 0xF5EE4BB9, 0xF8AD6D60, 0xFC6C70D7,
	0xE22B20D2, 0xE6EA3D65, 0xEBA91BBC, 0xEF68060B,
	0xD727BBB6, 0xD3E6A601, 0xDEA580D8, 0xDA649D6F,
	0xC423CD6A, 0xC0E2D0DD, 0xCDA1F604, 0xC960EBB3,
	0xBD3E8D7E, 0xB9FF90C9, 0xB4BCB610, 0xB07DABA7,
	0xAE3AFBA2, 0xAAFBE615, 0xA7B8C0CC, 0xA379DD7B,
	0x9B3660C6, 0x9FF77D71, 0x92B45BA8, 0x9675461F,
	0x8832161A, 0x8CF30BAD, 0x81B02D74, 0x857130C3,
	0x5D8A9099, 0x594B8D2E, 0x5408ABF7, 0x50C9B640,
	0x4E8EE645, 0x4A4FFBF2, 0x470CDD2B, 0x43CDC09C,
	0x7B827D21, 0x7F436096, 0x7200464F, 0x76C15BF8,
	0x68860BFD, 0x6C47164A, 0x61043093, 0x65C52D24,
	0x119B4BE9, 0x155A565E, 0x18197087, 0x1CD86D30,
	0x029F3D35, 0x065E2082, 0x0B1D065B, 0x0FDC1BEC,
	0x3793A651, 0x3352BBE6, 0x3E119D3F, 0x3AD08088,
	0x2497D08D, 0x2056CD3A, 0x2D15EBE3, 0x29D4F654,
	0xC5A92679, 0xC1683BCE, 0xCC2B1D17, 0xC8EA00A0,
	0xD6AD50A5, 0xD26C4D12, 0xDF2F6BCB, 0xDBEE767C,
	0xE3A1CBC1, 0xE760D676, 0xEA23F0AF, 0xEEE2ED18,
	0xF0A5BD1D, 0xF464A0AA, 0xF9278673, 0xFDE69BC4,
	0x89B8FD09, 0x8D79E0BE, 0x803AC667, 0x84FBDBD0,
	0x9ABC8BD5, 0x9E7D9662, 0x933EB0BB, 0x97FFAD0C,
	0xAFB010B1, 0xAB710D06, 0xA6322BDF, 0xA2F33668,
	0xBCB4666D, 0xB8757BDA, 0xB5365D03, 0xB1F740B4,
};

static uint32_t ts_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < len; i++)
		crc = (crc << 8) ^ ts_crc32_table[(crc >> 24) ^ data[i]];
	return crc;
}

/* ── 8-bit reversal table (SMPTE 302M bit ordering) ─────────────────── */

static const uint8_t ts_bit_reverse[256] = {
	0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
	0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
	0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
	0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
	0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4,
	0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
	0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC,
	0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
	0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
	0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
	0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
	0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
	0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
	0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
	0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
	0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
	0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1,
	0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
	0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9,
	0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
	0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5,
	0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
	0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED,
	0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
	0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3,
	0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
	0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB,
	0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
	0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7,
	0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
	0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF,
	0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF,
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v);
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)(v);
}

/* Write 5-byte PTS/DTS field (33-bit value with marker bits).
 * prefix_bits: 0010 for PTS-only, 0011 for PTS in PTS+DTS, 0001 for DTS. */
static void write_pts(uint8_t *p, uint8_t prefix_bits, uint64_t pts)
{
	p[0] = (uint8_t)((prefix_bits << 4) |
		(((pts >> 30) & 0x07) << 1) | 1);
	p[1] = (uint8_t)((pts >> 22) & 0xFF);
	p[2] = (uint8_t)((((pts >> 15) & 0x7F) << 1) | 1);
	p[3] = (uint8_t)((pts >> 7) & 0xFF);
	p[4] = (uint8_t)((((pts) & 0x7F) << 1) | 1);
}

/* Write a 6-byte PCR field (33-bit base + 6 reserved + 9-bit extension).
 * We set extension to 0. */
static void write_pcr(uint8_t *p, uint64_t pcr_base)
{
	p[0] = (uint8_t)(pcr_base >> 25);
	p[1] = (uint8_t)(pcr_base >> 17);
	p[2] = (uint8_t)(pcr_base >> 9);
	p[3] = (uint8_t)(pcr_base >> 1);
	p[4] = (uint8_t)(((pcr_base & 1) << 7) | 0x7E);  /* 6 reserved bits */
	p[5] = 0;  /* extension = 0 */
}

/* Start a TS packet header.  Returns pointer past the 4-byte header.
 * Sets payload_unit_start indicator if pusi is set. */
static uint8_t *start_ts_packet(uint8_t *pkt, uint16_t pid,
	uint8_t *cc, int pusi, int adapt)
{
	pkt[0] = TS_SYNC_BYTE;
	pkt[1] = (uint8_t)((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
	pkt[2] = (uint8_t)(pid & 0xFF);
	/* adaptation_field_control: 01=payload only, 11=adapt+payload, 10=adapt only */
	uint8_t afc = adapt ? 0x30 : 0x10;
	pkt[3] = (uint8_t)(afc | (*cc & 0x0F));
	*cc = (*cc + 1) & 0x0F;
	return pkt + 4;
}

void ts_mux_init(TsMuxState *s, uint32_t audio_rate, uint8_t audio_channels,
	uint8_t audio_codec)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->audio_rate = audio_rate;
	s->audio_channels = audio_channels;
	s->audio_codec = audio_codec;
	/* PCR leads PTS by ~1 frame at 30fps (3000 ticks at 90kHz) */
	s->pcr_offset = 3000;
	/* Force PAT/PMT emission on first frame */
	s->video_frames = TS_PAT_PMT_INTERVAL;
}

void ts_mux_reset_cc(TsMuxState *s)
{
	if (!s)
		return;
	s->cc_pat = 0;
	s->cc_pmt = 0;
	s->cc_video = 0;
	s->cc_audio = 0;
}

/* ── PAT / PMT ───────────────────────────────────────────────────────── */

size_t ts_mux_write_pat_pmt(TsMuxState *s, uint8_t *buf, size_t buf_size)
{
	uint8_t *p;
	uint8_t *section_start;
	uint32_t crc;
	size_t section_len;

	if (!s || !buf || buf_size < 2 * TS_PACKET_SIZE)
		return 0;

	/* ── PAT packet ────────────────────────────────────────────────── */
	memset(buf, 0xFF, TS_PACKET_SIZE);
	p = start_ts_packet(buf, TS_PID_PAT, &s->cc_pat, 1, 0);
	*p++ = 0x00;  /* pointer_field */

	section_start = p;
	*p++ = 0x00;  /* table_id = PAT */
	/* section_syntax_indicator=1, reserved bits, section_length placeholder */
	put_be16(p, 0xB00D);  /* 1011 0000 0000 1101 = syntax=1, len=13 */
	p += 2;
	put_be16(p, 0x0001);  /* transport_stream_id */
	p += 2;
	*p++ = 0xC1;  /* reserved, version=0, current_next=1 */
	*p++ = 0x00;  /* section_number */
	*p++ = 0x00;  /* last_section_number */
	/* Program 1 -> PMT PID */
	put_be16(p, 0x0001);  /* program_number */
	p += 2;
	put_be16(p, (uint16_t)(0xE000 | TS_PID_PMT));  /* reserved + PMT PID */
	p += 2;
	/* CRC32 over section (from table_id) */
	section_len = (size_t)(p - section_start);
	crc = ts_crc32(section_start, section_len);
	put_be32(p, crc);

	/* ── PMT packet ────────────────────────────────────────────────── */
	uint8_t *pmt = buf + TS_PACKET_SIZE;
	memset(pmt, 0xFF, TS_PACKET_SIZE);
	p = start_ts_packet(pmt, TS_PID_PMT, &s->cc_pmt, 1, 0);
	*p++ = 0x00;  /* pointer_field */

	section_start = p;
	*p++ = 0x02;  /* table_id = PMT */

	/* We'll fill in section_length after building the section */
	uint8_t *section_length_pos = p;
	p += 2;

	put_be16(p, 0x0001);  /* program_number */
	p += 2;
	*p++ = 0xC1;  /* reserved, version=0, current_next=1 */
	*p++ = 0x00;  /* section_number */
	*p++ = 0x00;  /* last_section_number */
	put_be16(p, (uint16_t)(0xE000 | TS_PID_VIDEO));  /* PCR PID = video */
	p += 2;
	put_be16(p, 0xF000);  /* program_info_length = 0 */
	p += 2;

	/* Video stream entry with HEVC registration descriptor */
	*p++ = TS_STREAM_TYPE_HEVC;
	put_be16(p, (uint16_t)(0xE000 | TS_PID_VIDEO));
	p += 2;
	put_be16(p, (uint16_t)(0xF000 | 6));  /* ES_info_length = 6 */
	p += 2;
	*p++ = 0x05;  /* descriptor_tag: registration_descriptor */
	*p++ = 4;     /* descriptor_length */
	*p++ = 'H'; *p++ = 'E'; *p++ = 'V'; *p++ = 'C';

	/* Audio stream entry — descriptors depend on codec:
	 *
	 *   SMPTE 302M PCM (BSSD): stream_type 0x06 + "BSSD" registration.
	 *     48 kHz only (302M requirement).  Mono inputs are upmixed to
	 *     stereo so number_channels >= 2.
	 *
	 *   Opus: stream_type 0x06 + "Opus" registration + extension
	 *     descriptor 0x7F/0x80 with channel_config_code (per the
	 *     Opus-in-MPEG-TS mapping; VLC 3+, ffmpeg, mpv, GStreamer).
	 */
	if (s->audio_rate > 0 && s->audio_channels > 0) {
		*p++ = TS_STREAM_TYPE_PRIVATE;
		put_be16(p, (uint16_t)(0xE000 | TS_PID_AUDIO));
		p += 2;

		if (s->audio_codec == TS_AUDIO_CODEC_OPUS) {
			put_be16(p, (uint16_t)(0xF000 | 10));  /* ES_info = 10 */
			p += 2;
			*p++ = 0x05;  /* registration_descriptor */
			*p++ = 4;
			*p++ = 'O'; *p++ = 'p'; *p++ = 'u'; *p++ = 's';
			*p++ = 0x7F;  /* extension_descriptor */
			*p++ = 2;
			*p++ = 0x80;  /* extension_descriptor_tag = Opus */
			/* channel_config_code: mapping family 0 is 1-based
			 *   0x00 = dual mono (NOT what we want for 1-channel input)
			 *   0x01 = mono, 0x02 = stereo, 0x03..0x08 = surround
			 * Clamp to 0x01..0x08; mono passes through as 1. */
			*p++ = s->audio_channels >= 1 && s->audio_channels <= 8
				? s->audio_channels : 1;
		} else {
			put_be16(p, (uint16_t)(0xF000 | 6));  /* ES_info = 6 */
			p += 2;
			*p++ = 0x05;
			*p++ = 4;
			*p++ = 'B'; *p++ = 'S'; *p++ = 'S'; *p++ = 'D';
		}
	}

	/* Fill in section_length: bytes from after section_length field to end
	 * including CRC (4 bytes) */
	section_len = (size_t)(p - (section_length_pos + 2)) + 4;
	put_be16(section_length_pos, (uint16_t)(0xB000 | (section_len & 0x0FFF)));

	/* CRC32 */
	section_len = (size_t)(p - section_start);
	crc = ts_crc32(section_start, section_len);
	put_be32(p, crc);

	return 2 * TS_PACKET_SIZE;
}

/* ── PES packetization ───────────────────────────────────────────────── */

/* Write PES-carrying TS packets for a given PID.
 * First packet includes PES header with PTS.
 * If pcr >= 0, first packet gets an adaptation field with PCR.
 * If rai is set, first packet sets random_access_indicator. */
static size_t write_pes_packets(TsMuxState *s, uint8_t *buf, size_t buf_size,
	uint16_t pid, uint8_t *cc, uint8_t stream_id,
	const uint8_t *data, size_t data_len,
	uint64_t pts_90khz, int use_pcr, int rai)
{
	size_t total = 0;
	size_t data_pos = 0;
	int first = 1;

	/* PES header: start code (3) + stream_id (1) + length (2) +
	 * flags (2) + header_data_length (1) + PTS (5) = 14 bytes */
	uint8_t pes_header[14];
	size_t pes_hdr_len = 14;

	pes_header[0] = 0x00;
	pes_header[1] = 0x00;
	pes_header[2] = 0x01;
	pes_header[3] = stream_id;
	/* PES packet length: 0 = unbounded (video) or actual for audio */
	size_t pes_payload = data_len + 8;  /* 8 = flags(2) + hdr_len(1) + PTS(5) */
	if (pes_payload > 0xFFFF)
		pes_payload = 0;  /* unbounded */
	put_be16(pes_header + 4, (uint16_t)pes_payload);
	pes_header[6] = 0x80;  /* '10' marker, no scrambling, no priority */
	pes_header[7] = 0x80;  /* PTS present, no DTS */
	pes_header[8] = 5;     /* PES header data length */
	write_pts(pes_header + 9, 2, pts_90khz);  /* prefix 0010 = PTS only */

	while (data_pos < data_len || first) {
		if (total + TS_PACKET_SIZE > buf_size)
			break;

		uint8_t *pkt = buf + total;
		memset(pkt, 0xFF, TS_PACKET_SIZE);

		if (first && (use_pcr || rai || s->discontinuity)) {
			/* First packet with adaptation field */
			uint8_t *p = start_ts_packet(pkt, pid, cc, 1, 1);
			uint8_t adapt_flags = 0;
			size_t adapt_len = 1;  /* flags byte */
			if (rai)
				adapt_flags |= 0x40;
			if (use_pcr) {
				adapt_flags |= 0x10;
				adapt_len += 6;
			}
			if (s->discontinuity) {
				adapt_flags |= 0x80;  /* discontinuity_indicator */
				s->discontinuity = 0;
			}
			*p++ = (uint8_t)adapt_len;
			*p++ = adapt_flags;
			if (use_pcr) {
				/* PCR leads PTS by pcr_offset (decoder buffer delay) */
				uint64_t pcr = pts_90khz;
				if (pcr > s->pcr_offset)
					pcr -= s->pcr_offset;
				write_pcr(p, pcr);
				p += 6;
			}

			/* Payload space after adaptation */
			size_t payload_space = (size_t)(TS_PACKET_SIZE - (p - pkt));

			/* Write PES header first, then data */
			size_t hdr_to_write = pes_hdr_len;
			if (hdr_to_write > payload_space)
				hdr_to_write = payload_space;
			memcpy(p, pes_header, hdr_to_write);
			p += hdr_to_write;
			payload_space -= hdr_to_write;

			size_t chunk = data_len - data_pos;
			if (chunk > payload_space)
				chunk = payload_space;
			if (chunk > 0)
				memcpy(p, data + data_pos, chunk);
			data_pos += chunk;
			first = 0;
		} else if (first) {
			/* First packet without adaptation field */
			uint8_t *p = start_ts_packet(pkt, pid, cc, 1, 0);
			size_t payload_space = (size_t)(TS_PACKET_SIZE - 4);

			size_t hdr_to_write = pes_hdr_len;
			if (hdr_to_write > payload_space)
				hdr_to_write = payload_space;
			memcpy(p, pes_header, hdr_to_write);
			p += hdr_to_write;
			payload_space -= hdr_to_write;

			size_t chunk = data_len - data_pos;
			if (chunk > payload_space)
				chunk = payload_space;
			if (chunk > 0)
				memcpy(p, data + data_pos, chunk);
			data_pos += chunk;
			first = 0;
		} else {
			/* Continuation packet */
			size_t remaining = data_len - data_pos;
			size_t payload_space = TS_PACKET_SIZE - 4;

			if (remaining < payload_space) {
				/* Need stuffing via adaptation field */
				size_t stuff = payload_space - remaining;
				uint8_t *p = start_ts_packet(pkt, pid, cc, 0, 1);
				if (stuff == 1) {
					*p++ = 0;  /* adapt_length = 0 */
				} else {
					*p++ = (uint8_t)(stuff - 1);
					*p++ = 0x00;  /* no flags */
					if (stuff > 2)
						memset(p, 0xFF, stuff - 2);
					p += (stuff > 2) ? (stuff - 2) : 0;
				}
				memcpy(p, data + data_pos, remaining);
				data_pos += remaining;
			} else {
				uint8_t *p = start_ts_packet(pkt, pid, cc, 0, 0);
				memcpy(p, data + data_pos, payload_space);
				data_pos += payload_space;
			}
		}

		total += TS_PACKET_SIZE;

		if (data_pos >= data_len && !first)
			break;
	}

	return total;
}

size_t ts_mux_write_video(TsMuxState *s, uint8_t *buf, size_t buf_size,
	const uint8_t *data, size_t data_len,
	uint64_t pts_90khz, int is_idr)
{
	size_t written = 0;

	if (!s || !buf || !data || data_len == 0)
		return 0;

	/* Emit PAT/PMT periodically */
	if (s->video_frames >= TS_PAT_PMT_INTERVAL) {
		size_t pat_pmt = ts_mux_write_pat_pmt(s, buf, buf_size);
		written += pat_pmt;
		s->video_frames = 0;
	}
	s->video_frames++;

	written += write_pes_packets(s, buf + written, buf_size - written,
		TS_PID_VIDEO, &s->cc_video, 0xE0,
		data, data_len, pts_90khz, 1, is_idr);

	return written;
}

/* SMPTE 302M PES payload buffer.
 * Worst-case input is one 65535-byte audio_ring entry, but in practice
 * star6e_audio.c pushes one MI_AI frame at a time (~20-40 ms @ 48 kHz =
 * 1920-3840 bytes mono s16le).  16-bit 302M packs each stereo pair as
 * 5 bytes (4 audio + 1 parity); cap at 16 KB input → ~40 KB AES3 output. */
#define TS_MUX_AUDIO_PES_MAX  (4 + 16384 * 5)

/* Opus-in-MPEG-TS access unit framing.  Each PES carries one or more Opus
 * packets prefixed by a control header:
 *   [11] control_header_prefix = 0x3FF (zero + ten 1s, packs as 0x7F 0xE0)
 *   [ 1] start_trim_flag = 0
 *   [ 1] end_trim_flag = 0
 *   [ 1] control_extension_flag = 0
 *   [ 2] reserved = 0
 *   [ 8N] au_size — (N-1) bytes of 0xFF + one byte < 0xFF; total = sum
 * ffmpeg's parser scans for 16-bit value 0x7FE0 (mask 0xFFE0) — see
 * libavcodec/opus/opus.h's OPUS_TS_HEADER / OPUS_TS_MASK.
 * For typical Opus packets (<255 B), au_size is one byte. */
static size_t write_opus_pes(TsMuxState *s, uint8_t *buf, size_t buf_size,
	const uint8_t *opus, size_t opus_len, uint64_t pts_90khz)
{
	uint8_t pes[2 + 32 + 4096];  /* hdr + au_size + Opus packet */
	size_t hdr_len = 2;
	size_t remaining = opus_len;

	if (opus_len + 32 + 2 > sizeof(pes))
		return 0;

	pes[0] = 0x7F;  /* prefix bits 10..3 (high byte of 0x7FE0) */
	pes[1] = 0xE0;  /* prefix bits 2..0 + 5 zero flag/reserved bits */

	while (remaining >= 0xFF) {
		pes[hdr_len++] = 0xFF;
		remaining -= 0xFF;
	}
	pes[hdr_len++] = (uint8_t)remaining;

	memcpy(pes + hdr_len, opus, opus_len);

	return write_pes_packets(s, buf, buf_size,
		TS_PID_AUDIO, &s->cc_audio, 0xBD,
		pes, hdr_len + opus_len, pts_90khz, 0, 0);
}

size_t ts_mux_write_audio(TsMuxState *s, uint8_t *buf, size_t buf_size,
	const uint8_t *data, size_t data_len,
	uint64_t pts_90khz)
{
	uint8_t aes_buf[TS_MUX_AUDIO_PES_MAX];
	const int16_t *samples;
	uint8_t *out;
	size_t in_frames;
	size_t aes_data_size;
	size_t pes_size;
	uint8_t in_channels;
	uint8_t out_channels;
	uint8_t out_pairs;
	uint8_t nch_code;
	int upmix_mono;

	if (!s || !buf || !data || data_len == 0)
		return 0;

	if (s->audio_codec == TS_AUDIO_CODEC_OPUS)
		return write_opus_pes(s, buf, buf_size, data, data_len, pts_90khz);

	if (data_len & 1)
		return 0;  /* SMPTE 302M needs whole 16-bit samples */

	in_channels = s->audio_channels ? s->audio_channels : 1;

	/* SMPTE 302M needs even channel counts (2/4/6/8).  Mono → upmix
	 * by duplicating L into R; otherwise round up to next even count
	 * (the extra channel gets zero samples — kept simple, won't trigger
	 * for 1/2-channel MI_AI captures). */
	upmix_mono = (in_channels == 1);
	out_channels = upmix_mono ? 2 :
		(uint8_t)((in_channels + 1) & ~1u);
	if (out_channels < 2 || out_channels > 8)
		return 0;
	out_pairs = (uint8_t)(out_channels / 2);

	in_frames = data_len / (sizeof(int16_t) * in_channels);
	if (in_frames == 0)
		return 0;

	/* 16-bit 302M layout per stereo sample frame (5 bytes, bit-packed).
	 *
	 *   byte[0] = bit_reverse_8(L & 0xFF)        — L low byte
	 *   byte[1] = bit_reverse_8((L >> 8) & 0xFF) — L high byte
	 *   byte[2] = bit_reverse_4(R & 0x0F)        — R low nibble in LOW nibble
	 *   byte[3] = bit_reverse_8((R >> 4) & 0xFF) — R middle byte
	 *   byte[4] = bit_reverse_4((R >> 12) & 0xF) << 4  — R high nibble in HIGH nibble
	 *
	 * Each AES3 sub-frame is 24 bits (4 sync bits + 16 audio + 4 status/parity);
	 * paired LR frames pack as 5 bytes total.  The ffmpeg decoder logic mirrored
	 * here lives in libavcodec/s302m.c.  Status/parity nibbles are emitted as 0.
	 *
	 * For N>2 channels: emit (N/2) consecutive 5-byte cells per frame. */
	aes_data_size = in_frames * out_pairs * 5;
	pes_size = 4 + aes_data_size;
	if (pes_size > sizeof(aes_buf))
		return 0;

	/* AES3 audio_data_header (32 bits, big-endian):
	 *   [16] audio_packet_size       — bytes following the header
	 *   [ 2] number_channels         — 00=2, 01=4, 10=6, 11=8
	 *   [ 8] channel_identification  — 0x00 (default)
	 *   [ 2] bits_per_sample         — 00=16, 01=20, 10=24
	 *   [ 4] alignment_bits          — 0
	 */
	nch_code = (uint8_t)(out_pairs - 1);
	put_be16(aes_buf, (uint16_t)aes_data_size);
	aes_buf[2] = (uint8_t)(nch_code << 6);  /* nch_code | (chan_id >> 2) */
	aes_buf[3] = 0x00;                       /* chan_id_lo | bps(16)=00 | align=0 */

	samples = (const int16_t *)data;
	out = aes_buf + 4;

	for (size_t i = 0; i < in_frames; i++) {
		uint8_t c = 0;
		uint8_t pair;
		for (pair = 0; pair < out_pairs; pair++) {
			uint16_t l = 0, r = 0;
			if (upmix_mono) {
				l = (uint16_t)samples[i];
				r = l;
			} else {
				if (c < in_channels)
					l = (uint16_t)samples[i * in_channels + c];
				c++;
				if (c < in_channels)
					r = (uint16_t)samples[i * in_channels + c];
				c++;
			}
			*out++ = ts_bit_reverse[l & 0xFF];
			*out++ = ts_bit_reverse[(l >> 8) & 0xFF];
			/* byte 2 low nibble = bit_reverse_4(R[3..0]); high nibble = 0
			 * Using the 8-bit table: place 4-bit value in top nibble of
			 * input → reverse swaps into bottom nibble of output. */
			*out++ = ts_bit_reverse[(r & 0x0F) << 4];
			*out++ = ts_bit_reverse[(r >> 4) & 0xFF];
			/* byte 4 high nibble = bit_reverse_4(R[15..12]); low nibble = 0
			 * Same trick, then shift the resulting 4-bit value back to
			 * the high nibble. */
			*out++ = (uint8_t)(ts_bit_reverse[((r >> 12) & 0x0F) << 4] << 4);
		}
	}

	return write_pes_packets(s, buf, buf_size,
		TS_PID_AUDIO, &s->cc_audio, 0xBD,
		aes_buf, pes_size, pts_90khz, 0, 0);
}
