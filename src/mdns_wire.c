/*
 * mdns_wire — pure DNS-SD / mDNS wire-format codec (RFC 6762 / RFC 6763).
 *
 * The DNS wire helpers here are derived from OpenIPC herald (MIT license),
 * the compact mDNS/DNS-SD stack for the OpenIPC project, via waybeam-hub's
 * mod_mdns:
 *   https://github.com/OpenIPC/firmware/tree/master/general/package/herald
 */

#include "mdns_wire.h"

#include <string.h>

/* ── Integer helpers ─────────────────────────────────────────────────── */

void mdns_put16(uint8_t *buf, uint16_t val)
{
	buf[0] = (uint8_t)(val >> 8);
	buf[1] = (uint8_t)(val & 0xFF);
}

void mdns_put32(uint8_t *buf, uint32_t val)
{
	buf[0] = (uint8_t)(val >> 24);
	buf[1] = (uint8_t)(val >> 16);
	buf[2] = (uint8_t)(val >> 8);
	buf[3] = (uint8_t)(val & 0xFF);
}

uint16_t mdns_get16(const uint8_t *buf)
{
	return (uint16_t)((uint16_t)buf[0] << 8 | (uint16_t)buf[1]);
}

/* ── Name encode / decode ────────────────────────────────────────────── */

int mdns_encode_name(uint8_t *buf, int buf_size, const char *name)
{
	int pos = 0;
	const char *p = name;

	while (*p) {
		const char *dot = strchr(p, '.');
		int label_len = dot ? (int)(dot - p) : (int)strlen(p);
		if (label_len <= 0 || label_len > 63)
			return -1;
		if (pos + 1 + label_len + 1 > buf_size)
			return -1;
		buf[pos++] = (uint8_t)label_len;
		memcpy(buf + pos, p, (size_t)label_len);
		pos += label_len;
		p += label_len;
		if (*p == '.') p++;
	}

	if (pos + 1 > buf_size) return -1;
	buf[pos++] = 0;  /* root label */
	return pos;
}

int mdns_decode_name(const uint8_t *pkt, int pkt_len, int offset,
	char *out, int out_size)
{
	int pos = offset;
	int out_pos = 0;
	int consumed = -1;  /* bytes consumed at original level */
	int jumps = 0;
	const int max_jumps = 16;

	while (pos < pkt_len && jumps < max_jumps) {
		uint8_t len = pkt[pos];

		if (len == 0) {
			if (consumed < 0) consumed = pos - offset + 1;
			break;
		}

		if ((len & 0xC0) == 0xC0) {
			/* Compression pointer */
			if (pos + 1 >= pkt_len) return -1;
			if (consumed < 0) consumed = pos - offset + 2;
			int ptr = ((len & 0x3F) << 8) | pkt[pos + 1];
			if (ptr >= pkt_len) return -1;
			pos = ptr;
			jumps++;
			continue;
		}

		if (pos + 1 + len > pkt_len) return -1;
		if (out_pos > 0 && out_pos < out_size - 1)
			out[out_pos++] = '.';

		int copy_len = len;
		if (out_pos + copy_len >= out_size - 1)
			copy_len = out_size - 1 - out_pos;
		if (copy_len > 0) {
			memcpy(out + out_pos, pkt + pos + 1, (size_t)copy_len);
			out_pos += copy_len;
		}

		pos += 1 + len;
	}

	if (out_pos < out_size)
		out[out_pos] = '\0';
	else if (out_size > 0)
		out[out_size - 1] = '\0';

	return consumed;
}

/* ── Resource-record builders ────────────────────────────────────────── */

int mdns_append_ptr(uint8_t *buf, int pos, int buf_size,
	const char *name, const char *target, uint32_t ttl)
{
	int name_len = mdns_encode_name(buf + pos, buf_size - pos, name);
	if (name_len < 0) return -1;
	int hdr_start = pos + name_len;
	if (hdr_start + 10 > buf_size) return -1;

	uint8_t rdata[MDNS_MAX_NAME];
	int rdata_len = mdns_encode_name(rdata, (int)sizeof(rdata), target);
	if (rdata_len < 0) return -1;
	if (hdr_start + 10 + rdata_len > buf_size) return -1;

	mdns_put16(buf + hdr_start, MDNS_TYPE_PTR);
	mdns_put16(buf + hdr_start + 2, MDNS_CLASS_IN);
	mdns_put32(buf + hdr_start + 4, ttl);
	mdns_put16(buf + hdr_start + 8, (uint16_t)rdata_len);
	memcpy(buf + hdr_start + 10, rdata, (size_t)rdata_len);

	return hdr_start + 10 + rdata_len;
}

int mdns_append_srv(uint8_t *buf, int pos, int buf_size,
	const char *name, const char *target_host, uint16_t port, uint32_t ttl)
{
	int name_len = mdns_encode_name(buf + pos, buf_size - pos, name);
	if (name_len < 0) return -1;
	int hdr_start = pos + name_len;
	if (hdr_start + 10 > buf_size) return -1;

	uint8_t rdata[MDNS_MAX_NAME + 6];
	mdns_put16(rdata, 0);      /* priority */
	mdns_put16(rdata + 2, 0);  /* weight   */
	mdns_put16(rdata + 4, port);
	int target_len = mdns_encode_name(rdata + 6, (int)sizeof(rdata) - 6,
		target_host);
	if (target_len < 0) return -1;
	int rdata_len = 6 + target_len;

	if (hdr_start + 10 + rdata_len > buf_size) return -1;

	mdns_put16(buf + hdr_start, MDNS_TYPE_SRV);
	mdns_put16(buf + hdr_start + 2, MDNS_CLASS_FLUSH);
	mdns_put32(buf + hdr_start + 4, ttl);
	mdns_put16(buf + hdr_start + 8, (uint16_t)rdata_len);
	memcpy(buf + hdr_start + 10, rdata, (size_t)rdata_len);

	return hdr_start + 10 + rdata_len;
}

int mdns_append_txt(uint8_t *buf, int pos, int buf_size,
	const char *name, const char *const *txt_pairs, uint32_t ttl)
{
	int name_len = mdns_encode_name(buf + pos, buf_size - pos, name);
	if (name_len < 0) return -1;
	int hdr_start = pos + name_len;
	if (hdr_start + 10 > buf_size) return -1;

	uint8_t rdata[MDNS_MAX_TXT_LEN];
	int rdata_len = 0;

	for (int i = 0; txt_pairs[i] != NULL; i++) {
		int pair_len = (int)strlen(txt_pairs[i]);
		if (pair_len > 255) pair_len = 255;
		if (rdata_len + 1 + pair_len > (int)sizeof(rdata)) break;
		rdata[rdata_len++] = (uint8_t)pair_len;
		memcpy(rdata + rdata_len, txt_pairs[i], (size_t)pair_len);
		rdata_len += pair_len;
	}

	/* Empty TXT must carry at least one zero-length string */
	if (rdata_len == 0)
		rdata[rdata_len++] = 0;

	if (hdr_start + 10 + rdata_len > buf_size) return -1;

	mdns_put16(buf + hdr_start, MDNS_TYPE_TXT);
	mdns_put16(buf + hdr_start + 2, MDNS_CLASS_FLUSH);
	mdns_put32(buf + hdr_start + 4, ttl);
	mdns_put16(buf + hdr_start + 8, (uint16_t)rdata_len);
	memcpy(buf + hdr_start + 10, rdata, (size_t)rdata_len);

	return hdr_start + 10 + rdata_len;
}

int mdns_append_a(uint8_t *buf, int pos, int buf_size,
	const char *name, const uint8_t addr[4], uint32_t ttl)
{
	int name_len = mdns_encode_name(buf + pos, buf_size - pos, name);
	if (name_len < 0) return -1;
	int hdr_start = pos + name_len;
	if (hdr_start + 10 + 4 > buf_size) return -1;

	mdns_put16(buf + hdr_start, MDNS_TYPE_A);
	mdns_put16(buf + hdr_start + 2, MDNS_CLASS_FLUSH);
	mdns_put32(buf + hdr_start + 4, ttl);
	mdns_put16(buf + hdr_start + 8, 4);
	memcpy(buf + hdr_start + 10, addr, 4);

	return hdr_start + 14;
}

/* ── Parsing ─────────────────────────────────────────────────────────── */

int mdns_parse_rr(const uint8_t *pkt, int pkt_len, int offset, MdnsRr *rr)
{
	int consumed = mdns_decode_name(pkt, pkt_len, offset,
		rr->name, (int)sizeof(rr->name));
	if (consumed < 0) return -1;
	int pos = offset + consumed;

	if (pos + 10 > pkt_len) return -1;
	rr->type = mdns_get16(pkt + pos);
	rr->rclass = mdns_get16(pkt + pos + 2);
	rr->ttl = (uint32_t)mdns_get16(pkt + pos + 4) << 16 | mdns_get16(pkt + pos + 6);
	rr->rdlength = mdns_get16(pkt + pos + 8);
	rr->rdata_offset = pos + 10;
	pos = rr->rdata_offset + rr->rdlength;

	if (pos > pkt_len) return -1;
	return pos;
}

bool mdns_txt_get_value(const uint8_t *rdata, int rdata_len,
	const char *key, char *out, int out_size)
{
	int pos = 0;
	int key_len = (int)strlen(key);

	while (pos < rdata_len) {
		uint8_t pair_len = rdata[pos++];
		if (pair_len == 0 || pos + pair_len > rdata_len) break;

		const char *pair = (const char *)(rdata + pos);
		if (pair_len >= key_len + 1 &&
			memcmp(pair, key, (size_t)key_len) == 0 &&
			pair[key_len] == '=') {
			int val_len = pair_len - key_len - 1;
			if (val_len >= out_size) val_len = out_size - 1;
			memcpy(out, pair + key_len + 1, (size_t)val_len);
			out[val_len] = '\0';
			return true;
		}
		pos += pair_len;
	}
	return false;
}
