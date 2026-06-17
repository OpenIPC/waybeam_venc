#include "mdns_beacon.h"

#include "device_id.h"
#include "mdns_wire.h"
#include "venc_config.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

/* ── Wire codec round-trip ───────────────────────────────────────────── */

static int test_wire_name_roundtrip(void)
{
	int failures = 0;
	uint8_t buf[128];
	int len = mdns_encode_name(buf, (int)sizeof(buf),
		"cam0._waybeam-venc._tcp.local");
	CHECK("wire_encode_name_ok", len > 0);

	char out[MDNS_MAX_NAME];
	int consumed = mdns_decode_name(buf, len, 0, out, (int)sizeof(out));
	CHECK("wire_decode_name_consumed", consumed == len);
	CHECK("wire_decode_name_match",
		strcmp(out, "cam0._waybeam-venc._tcp.local") == 0);

	/* Oversized label rejected */
	char big[80];
	memset(big, 'a', sizeof(big));
	big[70] = '\0';
	CHECK("wire_encode_long_label_rejected",
		mdns_encode_name(buf, (int)sizeof(buf), big) < 0);
	return failures;
}

static int test_wire_txt_get(void)
{
	int failures = 0;
	/* rdata: [7]"a=hello"[3]"b=1" */
	uint8_t rd[64];
	int n = 0;
	const char *p1 = "a=hello";
	const char *p2 = "b=1";
	rd[n++] = (uint8_t)strlen(p1);
	memcpy(rd + n, p1, strlen(p1)); n += (int)strlen(p1);
	rd[n++] = (uint8_t)strlen(p2);
	memcpy(rd + n, p2, strlen(p2)); n += (int)strlen(p2);

	char val[16] = "";
	CHECK("txt_get_a", mdns_txt_get_value(rd, n, "a", val, sizeof(val)) &&
		strcmp(val, "hello") == 0);
	CHECK("txt_get_b", mdns_txt_get_value(rd, n, "b", val, sizeof(val)) &&
		strcmp(val, "1") == 0);
	CHECK("txt_get_missing",
		!mdns_txt_get_value(rd, n, "c", val, sizeof(val)));
	return failures;
}

/* ── Beacon packet builder ───────────────────────────────────────────── */

static MdnsBeaconParams sample_params(void)
{
	MdnsBeaconParams p;
	memset(&p, 0, sizeof(p));
	p.enabled = true;
	snprintf(p.service_type, sizeof(p.service_type), "_waybeam-venc._tcp");
	snprintf(p.version, sizeof(p.version), "0.17.1");
	p.web_port = 80;
	return p;
}

static int test_build_packet_records(void)
{
	int failures = 0;
	MdnsBeaconParams p = sample_params();
	struct in_addr ip;
	CHECK("build_ip_parse", inet_pton(AF_INET, "192.168.1.42", &ip) == 1);

	uint8_t pkt[MDNS_BEACON_BUF_SIZE];
	int len = mdns_beacon_build_packet(pkt, (int)sizeof(pkt), &p, "cam0",
		&ip, 1, 120, NULL);
	CHECK("build_len_ok", len > 12);

	uint16_t qd = mdns_get16(pkt + 4);
	uint16_t an = mdns_get16(pkt + 6);
	CHECK("build_qd_zero", qd == 0);
	CHECK("build_answer_count", an == 4);  /* PTR + SRV + TXT + 1 A */

	bool ptr_ok = false, srv_ok = false, a_ok = false;
	bool txt_proto = false, txt_version = false;

	int pos = 12;
	for (uint16_t i = 0; i < an; i++) {
		MdnsRr rr;
		int next = mdns_parse_rr(pkt, len, pos, &rr);
		CHECK("build_parse_rr", next > pos);
		if (next <= pos) break;

		if (rr.type == MDNS_TYPE_PTR &&
			strcmp(rr.name, "_waybeam-venc._tcp.local") == 0)
			ptr_ok = true;

		if (rr.type == MDNS_TYPE_SRV && rr.rdlength >= 6) {
			uint16_t port = mdns_get16(pkt + rr.rdata_offset + 4);
			if (port == 80)
				srv_ok = true;
		}

		if (rr.type == MDNS_TYPE_A && rr.rdlength == 4) {
			if (memcmp(pkt + rr.rdata_offset, &ip.s_addr, 4) == 0)
				a_ok = true;
		}

		if (rr.type == MDNS_TYPE_TXT && rr.rdlength > 0) {
			const uint8_t *rd = pkt + rr.rdata_offset;
			int rl = rr.rdlength;
			char v[64] = "";
			txt_proto = mdns_txt_get_value(rd, rl, "proto", v, sizeof(v)) &&
				strcmp(v, "1") == 0;
			txt_version = mdns_txt_get_value(rd, rl, "version", v, sizeof(v)) &&
				strcmp(v, "0.17.1") == 0;
		}
		pos = next;
	}

	CHECK("build_has_ptr", ptr_ok);
	CHECK("build_has_srv_port", srv_ok);
	CHECK("build_has_a_ip", a_ok);
	CHECK("build_txt_proto", txt_proto);
	CHECK("build_txt_version", txt_version);

	/* Trimmed schema: sidecar_port/backend/model/codec/web_port/name are
	 * NOT advertised (probe-discoverable / GET /api/v1/config). */
	pos = 12;
	bool saw_dropped = false;
	for (uint16_t i = 0; i < an; i++) {
		MdnsRr rr;
		int next = mdns_parse_rr(pkt, len, pos, &rr);
		if (next <= pos) break;
		if (rr.type == MDNS_TYPE_TXT && rr.rdlength > 0) {
			char v[64] = "";
			const uint8_t *rd = pkt + rr.rdata_offset;
			int rl = rr.rdlength;
			saw_dropped =
				mdns_txt_get_value(rd, rl, "sidecar_port", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "backend", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "model", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "codec", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "web_port", v, sizeof(v)) ||
				mdns_txt_get_value(rd, rl, "name", v, sizeof(v));
		}
		pos = next;
	}
	CHECK("build_txt_no_dropped_keys", !saw_dropped);
	return failures;
}

static int test_build_packet_guards(void)
{
	int failures = 0;
	MdnsBeaconParams p = sample_params();
	struct in_addr ip;
	inet_pton(AF_INET, "10.0.0.1", &ip);
	uint8_t pkt[MDNS_BEACON_BUF_SIZE];

	CHECK("build_reject_no_ip",
		mdns_beacon_build_packet(pkt, sizeof(pkt), &p, "cam0", &ip, 0, 120, NULL) < 0);
	CHECK("build_reject_empty_host",
		mdns_beacon_build_packet(pkt, sizeof(pkt), &p, "", &ip, 1, 120, NULL) < 0);

	/* Goodbye packet (ttl 0): every record carries TTL 0 */
	int len = mdns_beacon_build_packet(pkt, sizeof(pkt), &p, "cam0", &ip, 1, 0, NULL);
	CHECK("build_goodbye_len", len > 12);
	uint16_t an = mdns_get16(pkt + 6);
	int pos = 12;
	bool all_ttl_zero = an > 0;
	for (uint16_t i = 0; i < an; i++) {
		MdnsRr rr;
		int next = mdns_parse_rr(pkt, len, pos, &rr);
		if (next <= pos) { all_ttl_zero = false; break; }
		if (rr.ttl != 0) all_ttl_zero = false;
		pos = next;
	}
	CHECK("build_goodbye_ttl_zero", all_ttl_zero);
	return failures;
}

/* ── Bare alias (waybeam.local) ──────────────────────────────────────── */

/* Count A records in pkt whose name equals want. */
static int count_a_records(const uint8_t *pkt, int len, const char *want)
{
	uint16_t an = mdns_get16(pkt + 6);
	int pos = 12, hits = 0;
	for (uint16_t i = 0; i < an; i++) {
		MdnsRr rr;
		int next = mdns_parse_rr(pkt, len, pos, &rr);
		if (next <= pos) break;
		if (rr.type == MDNS_TYPE_A && strcasecmp(rr.name, want) == 0)
			hits++;
		pos = next;
	}
	return hits;
}

static int test_bare_alias(void)
{
	int failures = 0;
	MdnsBeaconParams p = sample_params();
	struct in_addr ip;
	inet_pton(AF_INET, "192.168.1.42", &ip);
	uint8_t pkt[MDNS_BEACON_BUF_SIZE];

	/* No alias requested → only the primary host A record. */
	int len = mdns_beacon_build_packet(pkt, sizeof(pkt), &p,
		"waybeam-f5cb1d", &ip, 1, 120, NULL);
	CHECK("alias_absent_primary",
		count_a_records(pkt, len, "waybeam-f5cb1d.local") == 1);
	CHECK("alias_absent_bare",
		count_a_records(pkt, len, "waybeam.local") == 0);

	/* Alias requested → both primary and bare A records present. */
	len = mdns_beacon_build_packet(pkt, sizeof(pkt), &p,
		"waybeam-f5cb1d", &ip, 1, 120, "waybeam");
	CHECK("alias_present_primary",
		count_a_records(pkt, len, "waybeam-f5cb1d.local") == 1);
	CHECK("alias_present_bare",
		count_a_records(pkt, len, "waybeam.local") == 1);

	/* Alias == primary (Maruko bare case) → not duplicated. */
	len = mdns_beacon_build_packet(pkt, sizeof(pkt), &p,
		"waybeam", &ip, 1, 120, "waybeam");
	CHECK("alias_no_dup_when_equal",
		count_a_records(pkt, len, "waybeam.local") == 1);

	/* Tiebreak: higher IP wins, lower IP yields (RFC 6762 §8.2). */
	uint32_t lo, hi;
	inet_pton(AF_INET, "192.168.1.10", &lo);
	inet_pton(AF_INET, "192.168.1.20", &hi);
	CHECK("alias_tiebreak_low_yields", mdns_beacon_alias_loses(lo, hi));
	CHECK("alias_tiebreak_high_keeps", !mdns_beacon_alias_loses(hi, lo));
	return failures;
}

/* ── Config defaults ─────────────────────────────────────────────────── */

static int test_discovery_defaults(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	CHECK("disc_default_enabled", cfg.discovery.enabled == true);
	CHECK("disc_default_service",
		strcmp(cfg.discovery.service_type, "_waybeam-venc._tcp") == 0);
	CHECK("disc_default_name_empty", cfg.discovery.name[0] == '\0');
	CHECK("disc_default_bare_alias", cfg.discovery.bare_alias == true);
	return failures;
}

/* ── Device serial (die ID) ──────────────────────────────────────────── */

static int test_device_id(void)
{
	int failures = 0;
	char buf[16];

	/* Guards: NULL and too-small buffers must fail and not write garbage. */
	CHECK("dieid_null_rejected", !device_id_serial(NULL, sizeof(buf)));
	buf[0] = 'x';
	CHECK("dieid_short_buf_rejected", !device_id_serial(buf, 12));
	CHECK("dieid_short_buf_empty", buf[0] == '\0');

	/* Cached accessor never returns NULL; on a host without /dev/mem access
	 * it is the empty string.  When it does yield a value it must be 12 hex
	 * chars (the on-device path), never partial. */
	const char *s = device_id_serial_cached();
	CHECK("dieid_cached_nonnull", s != NULL);
	bool ok_shape = (s[0] == '\0') || (strlen(s) == 12);
	CHECK("dieid_cached_shape", ok_shape);
	CHECK("dieid_cached_stable", device_id_serial_cached() == s);
	return failures;
}

int test_mdns_beacon(void)
{
	int failures = 0;
	failures += test_wire_name_roundtrip();
	failures += test_wire_txt_get();
	failures += test_build_packet_records();
	failures += test_build_packet_guards();
	failures += test_bare_alias();
	failures += test_discovery_defaults();
	failures += test_device_id();
	return failures;
}
