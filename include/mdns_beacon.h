#ifndef MDNS_BEACON_H
#define MDNS_BEACON_H

/*
 * mdns_beacon — announce-only mDNS device beacon for waybeam_venc.
 *
 * Advertises this encoder as a `_waybeam-venc._tcp.local` service so that
 * ground stations and Android clients can discover the device independently
 * of the optional waybeam-hub.  The service type itself is the recognition
 * signal ("an OpenIPC camera running the waybeam encoder"); TXT is kept
 * deliberately minimal — only the waybeam `version` and the wire-schema
 * `proto`.  Everything else (SoC/backend, codec, sidecar port, full serial,
 * hub presence) is left for the consumer to fetch with one GET /api/v1/config
 * after discovery.  Live state (bitrate, fps, mode) is never advertised here.
 *
 * The instance/host name is `waybeam-<suffix>` where <suffix> is the tail of
 * the SoC die ID (device_id.h); see DISCOVERY_TRUST_MIGRATION_SPEC.md §4.5.
 * When `bare_alias` is set, the beacon also claims the bare `waybeam.local`
 * for single-device convenience, yielding it deterministically (lowest IP)
 * if another waybeam device contests it — see §4.6.
 *
 * The beacon runs on its own thread: it does not discover peers, does not
 * feed any trust layer, and never blocks the encode path.  It responds to
 * PTR queries for its own service type and re-announces periodically.
 */

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_BEACON_MAX_IPS 4
#define MDNS_BEACON_BUF_SIZE 1500

/* Resolved advertisement parameters (owned by value — no dangling pointers). */
typedef struct {
	bool     enabled;
	char     service_type[64];   /* "_waybeam-venc._tcp"        */
	char     name[64];           /* instance + hostname label   */
	char     version[24];        /* waybeam version (TXT)       */
	uint16_t web_port;           /* HTTP API port (SRV target)  */
	bool     bare_alias;         /* also claim bare "waybeam.local" (§4.6) */
} MdnsBeaconParams;

typedef struct {
	int                fd;             /* -1 = inert                     */
	bool               enabled;
	pthread_t          thread;
	bool               thread_started;
	volatile int       stop;
	MdnsBeaconParams   params;
	char               hostname[64];   /* resolved "<name>"              */
	char               alias_host[64]; /* bare "waybeam"; "" = no alias  */
	volatile int       alias_suppressed; /* lost the conflict tiebreak   */
	struct in_addr     local_ips[MDNS_BEACON_MAX_IPS];
	int                local_ip_count;
	struct sockaddr_in mcast_addr;
	uint8_t            response_buf[MDNS_BEACON_BUF_SIZE];
	int                response_len;
	uint8_t            goodbye_buf[MDNS_BEACON_BUF_SIZE];
	int                goodbye_len;
	uint8_t            alias_goodbye_buf[256]; /* A-only TTL=0 for alias  */
	int                alias_goodbye_len;
} MdnsBeacon;

/**
 * Build the full announce/response packet (PTR + SRV + TXT + A records)
 * into buf.  Pure and socket-free — fully unit-testable.  `hostname` is the
 * label used for the SRV target and A records ("<hostname>.local").  When
 * `extra_host` is non-NULL, non-empty, and differs from `hostname`, an extra
 * set of A records for "<extra_host>.local" (same IPs) is appended — the bare
 * "waybeam.local" convenience alias (§4.6).  ttl==0 builds a goodbye packet.
 * Returns the packet length, or -1 on error.
 */
int mdns_beacon_build_packet(uint8_t *buf, int buf_size,
	const MdnsBeaconParams *p, const char *hostname,
	const struct in_addr *ips, int n_ips, uint32_t ttl,
	const char *extra_host);

/**
 * Conflict tiebreak for the bare alias (RFC 6762 §8.2 lexicographic compare
 * of A rdata).  Returns true when WE should yield "waybeam.local" — i.e. the
 * peer's address is the lexicographically greater (winning) record.  Both
 * addresses are in network byte order.  Pure; exposed for testing.
 */
bool mdns_beacon_alias_loses(uint32_t our_addr_net, uint32_t their_addr_net);

/**
 * Start the beacon from resolved params: detect local IPv4s, build packets,
 * open the multicast socket, and spawn the servicing thread.  When
 * params->enabled is false (or no usable IP / socket error) the beacon stays
 * inert.  Returns 0 on success or inert, -1 on hard failure.
 */
int mdns_beacon_start(MdnsBeacon *b, const MdnsBeaconParams *p);

/**
 * Convenience wrapper: read the `discovery` settings and ports from a venc
 * config file, resolve the remaining params (version, hostname), and start.
 * Inert on any error.
 */
void mdns_beacon_start_from_config(MdnsBeacon *b, const char *config_path);

/** Stop the thread, multicast a goodbye, and close the socket.  Safe to call
 *  on an inert beacon or one that never started. */
void mdns_beacon_stop(MdnsBeacon *b);

#ifdef __cplusplus
}
#endif

#endif /* MDNS_BEACON_H */
