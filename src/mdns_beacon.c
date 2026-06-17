/*
 * mdns_beacon — announce-only mDNS device beacon for waybeam_venc.
 *
 * The multicast socket setup and announce/query-response pattern follow
 * OpenIPC herald (MIT license), the compact mDNS/DNS-SD stack for the
 * OpenIPC project:
 *   https://github.com/OpenIPC/firmware/tree/master/general/package/herald
 */

#include "mdns_beacon.h"

#include "device_id.h"
#include "mdns_wire.h"
#include "venc_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#ifndef VENC_VERSION
#define VENC_VERSION "unknown"
#endif

#define MDNS_BEACON_TTL              120
#define MDNS_BEACON_ANNOUNCE_COUNT     3
#define MDNS_BEACON_ANNOUNCE_DELAY_MS 250
#define MDNS_BEACON_REFRESH_MS      60000

/* Bounded copy — avoids snprintf format-truncation warnings when the source
 * may be longer than the destination (config strings → fixed params). */
static void copy_str(char *dst, size_t dst_sz, const char *src)
{
	if (dst_sz == 0)
		return;
	size_t i = 0;
	for (; src && src[i] && i < dst_sz - 1; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* ── Time ────────────────────────────────────────────────────────────── */

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ── Local IP detection (wireless interfaces first) ──────────────────── */

static int detect_local_ips(struct in_addr *out, int max_ips)
{
	struct ifaddrs *ifl = NULL;
	if (getifaddrs(&ifl) != 0)
		return 0;

	int count = 0;
	for (int pass = 0; pass < 2 && count < max_ips; pass++) {
		for (struct ifaddrs *ifa = ifl; ifa && count < max_ips;
			ifa = ifa->ifa_next) {
			if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
				continue;
			if (!(ifa->ifa_flags & IFF_UP) ||
				(ifa->ifa_flags & IFF_LOOPBACK))
				continue;
			bool is_wlan = ifa->ifa_name &&
				strncmp(ifa->ifa_name, "wlan", 4) == 0;
			if (pass == 0 && !is_wlan) continue;
			if (pass == 1 && is_wlan) continue;
			struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
			out[count++] = sin->sin_addr;
		}
	}

	freeifaddrs(ifl);
	return count;
}

/* ── Packet builder (pure) ───────────────────────────────────────────── */

int mdns_beacon_build_packet(uint8_t *buf, int buf_size,
	const MdnsBeaconParams *p, const char *hostname,
	const struct in_addr *ips, int n_ips, uint32_t ttl,
	const char *extra_host)
{
	if (!buf || !p || !hostname || hostname[0] == '\0')
		return -1;
	if (n_ips <= 0 || buf_size < 12)
		return -1;

	const char *svc_type = p->service_type[0] ? p->service_type
		: "_waybeam-venc._tcp";

	char svc_fqdn[MDNS_MAX_NAME];
	char inst_fqdn[MDNS_MAX_NAME];
	char host_fqdn[MDNS_MAX_NAME];
	snprintf(svc_fqdn, sizeof(svc_fqdn), "%s.local", svc_type);
	snprintf(inst_fqdn, sizeof(inst_fqdn), "%s.%s.local", hostname, svc_type);
	snprintf(host_fqdn, sizeof(host_fqdn), "%s.local", hostname);

	memset(buf, 0, 12);
	mdns_put16(buf + 2, MDNS_FLAG_QR | MDNS_FLAG_AA);

	int pos = 12;
	int answers = 0;
	int np;

	np = mdns_append_ptr(buf, pos, buf_size, svc_fqdn, inst_fqdn, ttl);
	if (np < 0)
		return -1;
	pos = np;
	answers++;

	np = mdns_append_srv(buf, pos, buf_size, inst_fqdn, host_fqdn,
		p->web_port, ttl);
	if (np < 0)
		return -1;
	pos = np;
	answers++;

	/* TXT — deliberately minimal (RFC 6763 §6).  The service type already
	 * says "waybeam encoder on an OpenIPC camera"; we add only the waybeam
	 * version and the wire-schema proto.  The sidecar port, full serial,
	 * backend/codec, and hub presence are fetched by the consumer with one
	 * GET /api/v1/config after discovery. */
	char proto_pair[16]   = "";
	char version_pair[40] = "";

	snprintf(proto_pair, sizeof(proto_pair), "proto=%d", MDNS_WIRE_VERSION);
	snprintf(version_pair, sizeof(version_pair), "version=%s",
		p->version[0] ? p->version : "unknown");

	const char *pairs[3];
	int n = 0;
	pairs[n++] = proto_pair;
	pairs[n++] = version_pair;
	pairs[n] = NULL;

	np = mdns_append_txt(buf, pos, buf_size, inst_fqdn, pairs, ttl);
	if (np < 0)
		return -1;
	pos = np;
	answers++;

	for (int i = 0; i < n_ips; i++) {
		uint8_t a[4];
		memcpy(a, &ips[i].s_addr, 4);
		np = mdns_append_a(buf, pos, buf_size, host_fqdn, a, ttl);
		if (np < 0)
			return -1;
		pos = np;
		answers++;
	}

	/* Bare convenience alias (§4.6): extra A records for "<extra_host>.local"
	 * → same IPs.  Skipped when it would duplicate the primary host name. */
	if (extra_host && extra_host[0] &&
		strcasecmp(extra_host, hostname) != 0) {
		char alias_fqdn[MDNS_MAX_NAME];
		snprintf(alias_fqdn, sizeof(alias_fqdn), "%s.local", extra_host);
		for (int i = 0; i < n_ips; i++) {
			uint8_t a[4];
			memcpy(a, &ips[i].s_addr, 4);
			np = mdns_append_a(buf, pos, buf_size, alias_fqdn, a, ttl);
			if (np < 0)
				return -1;
			pos = np;
			answers++;
		}
	}

	mdns_put16(buf + 6, (uint16_t)answers);
	return pos;
}

/* Build an A-only packet (just "<alias_host>.local" → IPs) for claiming or
 * (ttl=0) relinquishing the bare alias without touching the primary name. */
static int build_alias_only(uint8_t *buf, int buf_size, const char *alias_host,
	const struct in_addr *ips, int n_ips, uint32_t ttl)
{
	if (!buf || !alias_host || !alias_host[0] || n_ips <= 0 || buf_size < 12)
		return -1;
	char alias_fqdn[MDNS_MAX_NAME];
	snprintf(alias_fqdn, sizeof(alias_fqdn), "%s.local", alias_host);

	memset(buf, 0, 12);
	mdns_put16(buf + 2, MDNS_FLAG_QR | MDNS_FLAG_AA);
	int pos = 12, answers = 0;
	for (int i = 0; i < n_ips; i++) {
		uint8_t a[4];
		memcpy(a, &ips[i].s_addr, 4);
		int np = mdns_append_a(buf, pos, buf_size, alias_fqdn, a, ttl);
		if (np < 0)
			return -1;
		pos = np;
		answers++;
	}
	mdns_put16(buf + 6, (uint16_t)answers);
	return pos;
}

bool mdns_beacon_alias_loses(uint32_t our_addr_net, uint32_t their_addr_net)
{
	/* RFC 6762 §8.2: the lexicographically greater A rdata wins.  A-record
	 * bytes are network order, so a byte-string compare equals an unsigned
	 * compare of the host-order value.  We yield iff theirs is greater. */
	return ntohl(their_addr_net) > ntohl(our_addr_net);
}

/* ── Socket + send ───────────────────────────────────────────────────── */

static int beacon_open_socket(MdnsBeacon *b)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		fprintf(stderr, "[mdns_beacon] socket: %s\n", strerror(errno));
		return -1;
	}

	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

	struct sockaddr_in ba;
	memset(&ba, 0, sizeof(ba));
	ba.sin_family = AF_INET;
	ba.sin_port = htons(MDNS_PORT);
	ba.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&ba, sizeof(ba)) < 0) {
		fprintf(stderr, "[mdns_beacon] bind :%d: %s\n",
			MDNS_PORT, strerror(errno));
		close(fd);
		return -1;
	}

	uint8_t mc_ttl = 1;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &mc_ttl, sizeof(mc_ttl));
	uint8_t mc_loop = 0;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &mc_loop, sizeof(mc_loop));
	if (b->local_ip_count > 0)
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
			&b->local_ips[0], sizeof(b->local_ips[0]));

	struct ifaddrs *ifl = NULL;
	if (getifaddrs(&ifl) == 0) {
		for (struct ifaddrs *ifa = ifl; ifa; ifa = ifa->ifa_next) {
			if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
				continue;
			if (!(ifa->ifa_flags & IFF_UP) ||
				(ifa->ifa_flags & IFF_LOOPBACK))
				continue;
			if (!(ifa->ifa_flags & IFF_MULTICAST))
				continue;
			struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
			struct ip_mreq mreq;
			memset(&mreq, 0, sizeof(mreq));
			inet_pton(AF_INET, MDNS_GROUP, &mreq.imr_multiaddr);
			mreq.imr_interface = sin->sin_addr;
			setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				&mreq, sizeof(mreq));
		}
		freeifaddrs(ifl);
	}

	return fd;
}

static void beacon_send(MdnsBeacon *b, const uint8_t *buf, int len)
{
	if (b->fd < 0 || len <= 0)
		return;
	for (int i = 0; i < b->local_ip_count; i++) {
		setsockopt(b->fd, IPPROTO_IP, IP_MULTICAST_IF,
			&b->local_ips[i], sizeof(b->local_ips[i]));
		sendto(b->fd, buf, (size_t)len, 0,
			(struct sockaddr *)&b->mcast_addr, sizeof(b->mcast_addr));
	}
}

/* ── Query classification ────────────────────────────────────────────── */

static bool beacon_should_respond(const MdnsBeacon *b,
	const uint8_t *pkt, int len)
{
	if (len < 12)
		return false;
	if (mdns_get16(pkt + 2) & MDNS_FLAG_QR)
		return false;  /* a response, not a query */
	uint16_t qd = mdns_get16(pkt + 4);
	if (qd == 0)
		return false;

	const char *svc_type = b->params.service_type[0] ? b->params.service_type
		: "_waybeam-venc._tcp";
	char svc_fqdn[MDNS_MAX_NAME];
	char host_fqdn[MDNS_MAX_NAME];
	char alias_fqdn[MDNS_MAX_NAME];
	snprintf(svc_fqdn, sizeof(svc_fqdn), "%s.local", svc_type);
	snprintf(host_fqdn, sizeof(host_fqdn), "%s.local", b->hostname);
	if (b->alias_host[0] && !b->alias_suppressed)
		snprintf(alias_fqdn, sizeof(alias_fqdn), "%s.local", b->alias_host);
	else
		alias_fqdn[0] = '\0';

	int pos = 12;
	for (uint16_t i = 0; i < qd && pos < len; i++) {
		char qname[MDNS_MAX_NAME];
		int consumed = mdns_decode_name(pkt, len, pos, qname,
			(int)sizeof(qname));
		if (consumed < 0)
			return false;
		pos += consumed;
		if (pos + 4 > len)
			return false;
		uint16_t qtype = mdns_get16(pkt + pos);
		pos += 4;
		bool any = (qtype == 255);
		if ((qtype == MDNS_TYPE_PTR || any) &&
			strcasecmp(qname, svc_fqdn) == 0)
			return true;
		if ((qtype == MDNS_TYPE_A || any) &&
			strcasecmp(qname, host_fqdn) == 0)
			return true;
		if (alias_fqdn[0] && (qtype == MDNS_TYPE_A || any) &&
			strcasecmp(qname, alias_fqdn) == 0)
			return true;
	}
	return false;
}

/* Scan an incoming packet for another responder claiming our bare alias.
 * Returns that peer's IPv4 (network order) on conflict, else 0. */
static uint32_t beacon_scan_alias_conflict(const MdnsBeacon *b,
	const uint8_t *pkt, int len)
{
	if (!b->alias_host[0] || len < 12)
		return 0;
	char alias_fqdn[MDNS_MAX_NAME];
	snprintf(alias_fqdn, sizeof(alias_fqdn), "%s.local", b->alias_host);

	uint16_t qd = mdns_get16(pkt + 4);
	/* Counts are attacker-supplied; the real bound on both loops below is the
	 * `pos < len` gate, not these totals. */
	long rrs = (long)mdns_get16(pkt + 6) + mdns_get16(pkt + 8) +
		mdns_get16(pkt + 10);
	int pos = 12;

	for (uint16_t i = 0; i < qd && pos < len; i++) {
		char nm[MDNS_MAX_NAME];
		int c = mdns_decode_name(pkt, len, pos, nm, (int)sizeof(nm));
		if (c < 0)
			return 0;
		/* Bounds-check the qtype+qclass skip before advancing (mirror of
		 * beacon_should_respond) so pos can't run past the buffer. */
		if (pos + c + 4 > len)
			return 0;
		pos += c + 4;  /* qtype + qclass */
	}
	for (long i = 0; i < rrs && pos < len; i++) {
		MdnsRr rr;
		int next = mdns_parse_rr(pkt, len, pos, &rr);
		if (next <= pos)
			return 0;
		if (rr.type == MDNS_TYPE_A && rr.rdlength == 4 &&
			strcasecmp(rr.name, alias_fqdn) == 0) {
			uint32_t ip;
			memcpy(&ip, pkt + rr.rdata_offset, 4);
			bool mine = false;
			for (int k = 0; k < b->local_ip_count; k++)
				if (ip == b->local_ips[k].s_addr) {
					mine = true;
					break;
				}
			if (!mine)
				return ip;
		}
		pos = next;
	}
	return 0;
}

/* Rebuild the announce + goodbye packets for the current alias state. */
static void beacon_rebuild(MdnsBeacon *b)
{
	const char *alias = (b->alias_host[0] && !b->alias_suppressed)
		? b->alias_host : NULL;
	b->response_len = mdns_beacon_build_packet(b->response_buf,
		(int)sizeof(b->response_buf), &b->params, b->hostname,
		b->local_ips, b->local_ip_count, MDNS_BEACON_TTL, alias);
	b->goodbye_len = mdns_beacon_build_packet(b->goodbye_buf,
		(int)sizeof(b->goodbye_buf), &b->params, b->hostname,
		b->local_ips, b->local_ip_count, 0, alias);
}

/* ── Servicing thread ────────────────────────────────────────────────── */

static void *beacon_thread(void *arg)
{
	MdnsBeacon *b = (MdnsBeacon *)arg;
	int announce_sent = 0;
	uint64_t next_ms = now_ms();
	uint8_t rx[MDNS_BEACON_BUF_SIZE];

	while (!b->stop) {
		struct pollfd pfd = { .fd = b->fd, .events = POLLIN, .revents = 0 };
		int pr = poll(&pfd, 1, 200);
		uint64_t now = now_ms();

		if (announce_sent < MDNS_BEACON_ANNOUNCE_COUNT) {
			if (now >= next_ms) {
				beacon_send(b, b->response_buf, b->response_len);
				announce_sent++;
				next_ms = now + MDNS_BEACON_ANNOUNCE_DELAY_MS;
			}
		} else if (now >= next_ms) {
			beacon_send(b, b->response_buf, b->response_len);
			next_ms = now + MDNS_BEACON_REFRESH_MS;
		}

		if (pr > 0 && (pfd.revents & POLLIN)) {
			for (int drain = 0; drain < 8; drain++) {
				struct sockaddr_in from;
				socklen_t flen = sizeof(from);
				ssize_t n = recvfrom(b->fd, rx, sizeof(rx), 0,
					(struct sockaddr *)&from, &flen);
				if (n <= 0)
					break;
				if (n < 12)
					continue;
				bool self = false;
				for (int i = 0; i < b->local_ip_count; i++) {
					if (from.sin_addr.s_addr ==
						b->local_ips[i].s_addr) {
						self = true;
						break;
					}
				}
				if (self)
					continue;

				/* Bare-alias conflict resolution (§4.6): if a peer claims
				 * our alias, the lower IP yields it (the other side keeps
				 * it).  We still answer to our unique suffixed name.  We
				 * advertise the alias on every local IP, so the §8.2
				 * tiebreak must use our *highest* address (the greatest of
				 * our own records the peer is contesting). */
				if (b->alias_host[0] && !b->alias_suppressed) {
					uint32_t peer =
						beacon_scan_alias_conflict(b, rx, (int)n);
					uint32_t our_max = b->local_ips[0].s_addr;
					for (int k = 1; k < b->local_ip_count; k++)
						if (ntohl(b->local_ips[k].s_addr) >
							ntohl(our_max))
							our_max = b->local_ips[k].s_addr;
					if (peer && mdns_beacon_alias_loses(our_max, peer)) {
						beacon_send(b, b->alias_goodbye_buf,
							b->alias_goodbye_len);
						b->alias_suppressed = 1;
						beacon_rebuild(b);
						fprintf(stderr, "[mdns_beacon] %s.local contested "
							"— yielding alias\n", b->alias_host);
					} else if (peer) {
						/* We win — re-assert immediately to defend. */
						beacon_send(b, b->response_buf, b->response_len);
					}
				}

				if (beacon_should_respond(b, rx, (int)n))
					beacon_send(b, b->response_buf, b->response_len);
			}
		}
	}
	return NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int mdns_beacon_start(MdnsBeacon *b, const MdnsBeaconParams *p)
{
	if (!b)
		return -1;
	memset(b, 0, sizeof(*b));
	b->fd = -1;

	if (!p || !p->enabled)
		return 0;  /* inert */

	b->params = *p;
	if (!b->params.service_type[0])
		snprintf(b->params.service_type, sizeof(b->params.service_type),
			"_waybeam-venc._tcp");

	if (p->name[0]) {
		snprintf(b->hostname, sizeof(b->hostname), "%s", p->name);
	} else {
		char hn[64] = "";
		if (gethostname(hn, sizeof(hn) - 1) != 0 || hn[0] == '\0')
			snprintf(hn, sizeof(hn), "waybeam");
		snprintf(b->hostname, sizeof(b->hostname), "%s", hn);
	}

	/* Bare convenience alias (§4.6): claim "waybeam.local" only when the
	 * primary name is something else (i.e. a suffixed device).  If the
	 * primary already is "waybeam" there is nothing extra to claim. */
	if (b->params.bare_alias && strcmp(b->hostname, "waybeam") != 0)
		snprintf(b->alias_host, sizeof(b->alias_host), "waybeam");

	b->local_ip_count = detect_local_ips(b->local_ips, MDNS_BEACON_MAX_IPS);
	if (b->local_ip_count <= 0) {
		fprintf(stderr, "[mdns_beacon] no local IPv4 — beacon inert\n");
		return 0;  /* inert, not fatal */
	}

	beacon_rebuild(b);
	if (b->alias_host[0])
		b->alias_goodbye_len = build_alias_only(b->alias_goodbye_buf,
			(int)sizeof(b->alias_goodbye_buf), b->alias_host,
			b->local_ips, b->local_ip_count, 0);
	if (b->response_len <= 0 || b->goodbye_len <= 0) {
		fprintf(stderr, "[mdns_beacon] failed to build packets\n");
		return -1;
	}

	memset(&b->mcast_addr, 0, sizeof(b->mcast_addr));
	b->mcast_addr.sin_family = AF_INET;
	b->mcast_addr.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_GROUP, &b->mcast_addr.sin_addr);

	b->fd = beacon_open_socket(b);
	if (b->fd < 0) {
		fprintf(stderr, "[mdns_beacon] socket setup failed — beacon inert\n");
		return 0;  /* inert */
	}

	b->enabled = true;
	b->stop = 0;
	if (pthread_create(&b->thread, NULL, beacon_thread, b) != 0) {
		fprintf(stderr, "[mdns_beacon] thread create failed: %s\n",
			strerror(errno));
		close(b->fd);
		b->fd = -1;
		b->enabled = false;
		return -1;
	}
	b->thread_started = true;

	printf("> mDNS beacon: %s.%s.local web:%u (%d ip)%s%s\n",
		b->hostname, b->params.service_type,
		(unsigned)b->params.web_port, b->local_ip_count,
		b->alias_host[0] ? " + alias " : "",
		b->alias_host[0] ? "waybeam.local" : "");
	return 0;
}

void mdns_beacon_start_from_config(MdnsBeacon *b, const char *config_path)
{
	VencConfig cfg;
	venc_config_defaults(&cfg);
	if (config_path)
		(void)venc_config_load(config_path, &cfg);

	MdnsBeaconParams p;
	memset(&p, 0, sizeof(p));
	p.enabled = cfg.discovery.enabled;
	copy_str(p.service_type, sizeof(p.service_type),
		cfg.discovery.service_type[0] ? cfg.discovery.service_type
			: "_waybeam-venc._tcp");

	/* Name resolution (§4.1/§4.5): operator override → serial-suffixed
	 * waybeam-<die-id tail> → bare waybeam.  The SoC die ID gives every
	 * Star6E a stable, collision-free name with no RFC conflict churn;
	 * Maruko (no die ID) lands on bare waybeam unless pinned via config. */
	if (cfg.discovery.name[0]) {
		copy_str(p.name, sizeof(p.name), cfg.discovery.name);
	} else {
		const char *serial = device_id_serial_cached();
		size_t sl = strlen(serial);
		if (sl >= 6) {
			const char *suf = serial + sl - 6;  /* last 6 hex */
			char low[8];
			for (int i = 0; i < 6; i++)
				low[i] = (char)tolower((unsigned char)suf[i]);
			low[6] = '\0';
			snprintf(p.name, sizeof(p.name), "waybeam-%s", low);
		} else {
			snprintf(p.name, sizeof(p.name), "waybeam");
		}
	}

	snprintf(p.version, sizeof(p.version), "%s", VENC_VERSION);
	p.web_port = cfg.system.web_port;
	p.bare_alias = cfg.discovery.bare_alias;

	(void)mdns_beacon_start(b, &p);
}

void mdns_beacon_stop(MdnsBeacon *b)
{
	if (!b)
		return;

	if (b->thread_started) {
		b->stop = 1;
		pthread_join(b->thread, NULL);
		b->thread_started = false;
	}

	for (int i = 0; i < 2 && b->fd >= 0 && b->goodbye_len > 0; i++) {
		beacon_send(b, b->goodbye_buf, b->goodbye_len);
		if (i == 0)
			usleep(20000);
	}

	if (b->fd >= 0) {
		close(b->fd);
		b->fd = -1;
	}
	b->enabled = false;
}
