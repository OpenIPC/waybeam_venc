#include "ts_mux.h"

#include <string.h>

/* ── CRC32 for MPEG-TS (polynomial 0x04C11DB7) ──────────────────────── */

static uint32_t ts_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x80000000)
				crc = (crc << 1) ^ 0x04C11DB7;
			else
				crc <<= 1;
		}
	}
	return crc;
}

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

void ts_mux_init(TsMuxState *s, uint32_t audio_rate, uint8_t audio_channels)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->audio_rate = audio_rate;
	s->audio_channels = audio_channels;
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

	/* Audio stream entry: private data with LPCM registration descriptor */
	if (s->audio_rate > 0 && s->audio_channels > 0) {
		*p++ = TS_STREAM_TYPE_PRIVATE;
		put_be16(p, (uint16_t)(0xE000 | TS_PID_AUDIO));
		p += 2;
		/* Registration descriptor: format_identifier "LPCM" */
		put_be16(p, (uint16_t)(0xF000 | 6));  /* ES_info_length = 6 */
		p += 2;
		*p++ = 0x05;  /* descriptor_tag: registration_descriptor */
		*p++ = 4;     /* descriptor_length */
		*p++ = 'L'; *p++ = 'P'; *p++ = 'C'; *p++ = 'M';
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

size_t ts_mux_write_audio(TsMuxState *s, uint8_t *buf, size_t buf_size,
	const uint8_t *data, size_t data_len,
	uint64_t pts_90khz)
{
	if (!s || !buf || !data || data_len == 0)
		return 0;

	return write_pes_packets(s, buf, buf_size,
		TS_PID_AUDIO, &s->cc_audio, 0xBD,
		data, data_len, pts_90khz, 0, 0);
}
