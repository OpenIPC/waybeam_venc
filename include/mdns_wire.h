#ifndef MDNS_WIRE_H
#define MDNS_WIRE_H

/*
 * mdns_wire — pure DNS-SD / mDNS wire-format codec (RFC 6762 / RFC 6763).
 *
 * Self-contained, socket-free helpers for encoding and parsing the DNS
 * resource records used by Waybeam service discovery.  Adapted from the
 * waybeam-hub mod_mdns implementation, whose DNS wire helpers are in turn
 * derived from OpenIPC herald (MIT license) — the compact mDNS/DNS-SD stack
 * for the OpenIPC project:
 *   https://github.com/OpenIPC/firmware/tree/master/general/package/herald
 *
 * This file is the source of truth for the shared wire codec.  waybeam-hub
 * vendors a copy; bump MDNS_WIRE_VERSION and keep both in sync when the
 * encoding changes.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_WIRE_VERSION 1

/* Multicast endpoint */
#define MDNS_PORT          5353
#define MDNS_GROUP         "224.0.0.251"

/* Buffer bounds */
#define MDNS_MAX_NAME      256
#define MDNS_MAX_TXT_LEN   512

/* DNS record types */
#define MDNS_TYPE_A     1
#define MDNS_TYPE_PTR  12
#define MDNS_TYPE_TXT  16
#define MDNS_TYPE_SRV  33

/* DNS header flags */
#define MDNS_FLAG_QR     0x8000  /* response                 */
#define MDNS_FLAG_AA     0x0400  /* authoritative            */

/* DNS class */
#define MDNS_CLASS_IN    1
#define MDNS_CLASS_FLUSH 0x8001  /* cache-flush + IN         */

/* ── Integer helpers (big-endian wire order) ─────────────────────────── */

void     mdns_put16(uint8_t *buf, uint16_t val);
void     mdns_put32(uint8_t *buf, uint32_t val);
uint16_t mdns_get16(const uint8_t *buf);

/* ── Name encode / decode ────────────────────────────────────────────── */

/** Encode "foo.bar.local" → [3]foo[3]bar[5]local[0].  Returns bytes
 *  written, or -1 on overflow / invalid label. */
int mdns_encode_name(uint8_t *buf, int buf_size, const char *name);

/** Decode a (possibly compressed) DNS name at offset into out.  Returns
 *  bytes consumed at the original level (pointers not followed), or -1. */
int mdns_decode_name(const uint8_t *pkt, int pkt_len, int offset,
	char *out, int out_size);

/* ── Resource-record builders ────────────────────────────────────────── */

/** Append a PTR record (service_type → instance).  Returns the new buffer
 *  position, or -1 on overflow. */
int mdns_append_ptr(uint8_t *buf, int pos, int buf_size,
	const char *name, const char *target, uint32_t ttl);

/** Append an SRV record (instance → host:port). */
int mdns_append_srv(uint8_t *buf, int pos, int buf_size,
	const char *name, const char *target_host, uint16_t port, uint32_t ttl);

/** Append a TXT record from a NULL-terminated array of "key=value" pairs. */
int mdns_append_txt(uint8_t *buf, int pos, int buf_size,
	const char *name, const char *const *txt_pairs, uint32_t ttl);

/** Append an A record (host → IPv4, 4-byte address in network order). */
int mdns_append_a(uint8_t *buf, int pos, int buf_size,
	const char *name, const uint8_t addr[4], uint32_t ttl);

/* ── Parsing ─────────────────────────────────────────────────────────── */

typedef struct {
	char     name[MDNS_MAX_NAME];
	uint16_t type;
	uint16_t rclass;
	uint32_t ttl;
	uint16_t rdlength;
	int      rdata_offset;
} MdnsRr;

/** Parse one resource record at offset into rr.  Returns the offset after
 *  the record, or -1 on malformed input. */
int mdns_parse_rr(const uint8_t *pkt, int pkt_len, int offset, MdnsRr *rr);

/** Extract a TXT key's value from rdata ([len][key=val]...).  Returns true
 *  if found and copies the value (NUL-terminated) into out. */
bool mdns_txt_get_value(const uint8_t *rdata, int rdata_len,
	const char *key, char *out, int out_size);

#ifdef __cplusplus
}
#endif

#endif /* MDNS_WIRE_H */
