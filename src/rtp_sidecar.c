#include "rtp_sidecar.h"
#include "timing.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* htobe64 requires _DEFAULT_SOURCE on glibc / musl under -std=c99.
 * Provide a portable fallback that works on any endianness by
 * constructing big-endian bytes directly from arithmetic shifts. */
static inline uint64_t sidecar_htobe64(uint64_t v)
{
	uint8_t b[8];
	b[0] = (uint8_t)(v >> 56);
	b[1] = (uint8_t)(v >> 48);
	b[2] = (uint8_t)(v >> 40);
	b[3] = (uint8_t)(v >> 32);
	b[4] = (uint8_t)(v >> 24);
	b[5] = (uint8_t)(v >> 16);
	b[6] = (uint8_t)(v >> 8);
	b[7] = (uint8_t)(v);
	uint64_t r;
	memcpy(&r, b, sizeof(r));
	return r;
}

/* ── Subscriber table (protocols/rtp-sidecar.md) ─────────────────────────
 *
 * Up to RTP_SIDECAR_MAX_SUBS consumers keyed by (addr, port), each with
 * its own TTL. SUBSCRIBE/SYNC_REQ claims or refreshes the matching slot;
 * FRAME fans out to every live slot. A single subscriber behaves exactly
 * like the historical single-slot implementation. */

static int slot_live_at(const RtpSidecarSub *sub, uint64_t now)
{
	return sub->expires_us != 0 && now < sub->expires_us;
}

static int sub_active_at(const RtpSidecarSender *s, uint64_t now)
{
	for (int i = 0; i < RTP_SIDECAR_MAX_SUBS; i++)
		if (slot_live_at(&s->subs[i], now))
			return 1;
	return 0;
}

int rtp_sidecar_is_subscribed(const RtpSidecarSender *s)
{
	if (!s || s->fd < 0)
		return 0;
	return sub_active_at(s, wb_monotonic_us());
}

static int addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
	       a->sin_port == b->sin_port;
}

/* Claim or refresh the slot for src. Returns 1 if this is a NEW
 * subscriber (for logging), 0 on refresh or table-full drop. */
static int sub_refresh_at(RtpSidecarSender *s, const struct sockaddr_in *src,
	uint64_t now)
{
	RtpSidecarSub *free_slot = NULL;

	for (int i = 0; i < RTP_SIDECAR_MAX_SUBS; i++) {
		RtpSidecarSub *sub = &s->subs[i];
		if (slot_live_at(sub, now)) {
			if (addr_eq(&sub->addr, src)) {
				sub->expires_us = now + RTP_SIDECAR_SUB_TTL_US;
				return 0;
			}
		} else if (!free_slot) {
			free_slot = sub;
		}
	}
	if (!free_slot) {
		static int full_warned;
		if (!full_warned) {
			full_warned = 1;
			fprintf(stderr, "[sidecar] subscriber table full (%d); "
			        "ignoring %s:%u\n", RTP_SIDECAR_MAX_SUBS,
			        inet_ntoa(src->sin_addr), ntohs(src->sin_port));
		}
		return 0;
	}
	free_slot->addr       = *src;
	free_slot->expires_us = now + RTP_SIDECAR_SUB_TTL_US;
	return 1;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int rtp_sidecar_sender_init(RtpSidecarSender *s, uint16_t sidecar_port)
{
	if (!s)
		return -1;

	s->fd       = -1;
	s->frame_id = 0;
	memset(s->subs, 0, sizeof(s->subs));

	if (sidecar_port == 0)
		return 0;  /* disabled — not an error */

	/* SOCK_CLOEXEC: must NOT survive execv() into the SIGHUP-respawn child.
	 * AF_INET UDP + SO_REUSEADDR lets the child rebind 5602 alongside any
	 * inherited stale fd, so without CLOEXEC each respawn cycle accumulates
	 * one extra listener — the kernel's reuseaddr distribution then routes
	 * subscribe packets to a socket no thread is reading. */
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "[sidecar] socket: %s\n", strerror(errno));
		return -1;
	}

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	/* Non-blocking: poll() in the encode loop must never stall */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		fprintf(stderr, "[sidecar] fcntl O_NONBLOCK: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	struct sockaddr_in local = {
		.sin_family      = AF_INET,
		.sin_port        = htons(sidecar_port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		fprintf(stderr, "[sidecar] bind :%u: %s\n",
		        sidecar_port, strerror(errno));
		close(fd);
		return -1;
	}

	s->fd = fd;
	fprintf(stderr, "[sidecar] listening on :%u (waiting for probe)\n",
	        sidecar_port);
	return 0;
}

void rtp_sidecar_sender_close(RtpSidecarSender *s)
{
	/* Skip on fd <= 0: -1 means already closed; 0 means a freshly-zeroed
	 * struct (calloc / BSS / memset) where the field has never been
	 * assigned a real socket.  fd 0 is stdin in this codebase — the
	 * sidecar listener is always >= 3 — so treating 0 as "uninitialised"
	 * lets star6e_video_init call this defensively before its own memset
	 * without closing stdin on the first invocation. */
	if (!s || s->fd <= 0)
		return;
	close(s->fd);
	s->fd = -1;
}

/* ── Inbound poll: subscribe + sync ─────────────────────────────────── */

void rtp_sidecar_poll(RtpSidecarSender *s)
{
	if (!s || s->fd < 0)
		return;

	/*
	 * Drain all pending datagrams in one call so we don't fall behind
	 * at high subscribe/sync rates, but cap iterations to avoid
	 * spending too long in the encode loop.
	 *
	 * One `wb_monotonic_us()` sample per drained datagram is enough — control
	 * traffic is rare (subscribe every 2 s, sync every 200-10000 ms).
	 * Only the SYNC_RESP case still needs an extra sample for t3 since
	 * it represents "just before sendto".
	 */
	for (int iter = 0; iter < 8; iter++) {
		uint8_t buf[64];
		struct sockaddr_in src;
		socklen_t src_len = sizeof(src);

		ssize_t n = recvfrom(s->fd, buf, sizeof(buf), MSG_DONTWAIT,
		                     (struct sockaddr *)&src, &src_len);
		if (n < 0)
			break;  /* EAGAIN or real error — both non-fatal */

		if (n < 6)
			continue;

		const uint32_t *magic_p = (const uint32_t *)buf;
		if (ntohl(*magic_p) != RTP_SIDECAR_MAGIC)
			continue;
		if (buf[4] != RTP_SIDECAR_VERSION)
			continue;

		uint8_t msg_type = buf[5];
		uint64_t now = wb_monotonic_us();

		switch (msg_type) {

		case RTP_SIDECAR_MSG_SUBSCRIBE:
			/* Any subscribe (including keepalive) refreshes the TTL */
			if (sub_refresh_at(s, &src, now))
				fprintf(stderr, "[sidecar] probe subscribed from %s:%u\n",
				        inet_ntoa(src.sin_addr), ntohs(src.sin_port));
			break;

		case RTP_SIDECAR_MSG_SYNC_REQ:
			if ((size_t)n < sizeof(RtpSidecarSyncReq))
				continue;
			{
				const RtpSidecarSyncReq *req = (const RtpSidecarSyncReq *)buf;

				RtpSidecarSyncResp resp;
				resp.magic    = htonl(RTP_SIDECAR_MAGIC);
				resp.version  = RTP_SIDECAR_VERSION;
				resp.msg_type = RTP_SIDECAR_MSG_SYNC_RESP;
				resp._pad[0]  = 0;
				resp._pad[1]  = 0;
				resp.t1_us    = req->t1_us;  /* echo verbatim */
				resp.t2_us    = sidecar_htobe64(now);
				resp.t3_us    = sidecar_htobe64(wb_monotonic_us());

				sendto(s->fd, &resp, sizeof(resp), MSG_DONTWAIT,
				       (struct sockaddr *)&src, src_len);
				/* Also refresh subscriber so sync acts as keepalive */
				sub_refresh_at(s, &src, now);
			}
			break;

		default:
			break;
		}
	}
}

/* ── Frame metadata sender ───────────────────────────────────────────── */

/* Assemble one FRAME datagram (base + flag-ordered trailers) into a stack
 * buffer and fan it out to every live subscriber.  Trailers are appended
 * sequentially in flag-bit order, so an absent trailer simply leaves the next
 * one sliding up — no gaps, computable offsets.  DETECT is the only variable-
 * length trailer and is appended last; the caller passes a fully serialised
 * blob (header + TLV body, network byte order) which is copied verbatim, so
 * the sidecar never parses detection tags.  A raw byte buffer (not the fixed
 * RtpSidecarFrameExtFull struct) is used precisely so the variable DETECT blob
 * fits. */
static int send_frame_impl(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info,
	const RtpSidecarAttitudeInfo *attitude_info,
	const void *detect_blob, uint16_t detect_len)
{
	if (!s || s->fd < 0)
		return 0;  /* disabled */

	/* One clock sample covers both the subscription check and the
	 * last_pkt_send_us field. The caller has just returned from the
	 * last RTP sendmsg(), so sampling at function entry is what we
	 * want — not sampling again a few microseconds later inside the
	 * serialiser. Saves one clock_gettime per frame. */
	uint64_t now = wb_monotonic_us();
	if (!sub_active_at(s, now))
		return 0;  /* no subscriber — channel stays silent */

	uint8_t buf[RTP_SIDECAR_DGRAM_MAX];
	RtpSidecarFrame frame;
	uint8_t flags = 0;
	size_t off = sizeof(frame);

	memset(&frame, 0, sizeof(frame));
	frame.magic            = htonl(RTP_SIDECAR_MAGIC);
	frame.version          = RTP_SIDECAR_VERSION;
	frame.msg_type         = RTP_SIDECAR_MSG_FRAME;
	frame.stream_id        = 0;
	frame.ssrc             = htonl(ssrc);
	frame.rtp_timestamp    = htonl(rtp_ts);
	frame.frame_id         = sidecar_htobe64(s->frame_id++);
	frame.frame_ready_us   = sidecar_htobe64(frame_ready_us);
	frame.seq_first        = htons(seq_first);
	frame.seq_count        = htons(seq_count);
	frame.capture_us       = sidecar_htobe64(capture_us);
	frame.last_pkt_send_us = sidecar_htobe64(now);

	if (enc_info) {
		RtpSidecarEncInfoWire wire;

		flags |= RTP_SIDECAR_FLAG_ENC_INFO;
		if (enc_info->frame_type == RTP_SIDECAR_FRAME_I ||
		    enc_info->frame_type == RTP_SIDECAR_FRAME_IDR)
			flags |= RTP_SIDECAR_FLAG_KEYFRAME;

		memset(&wire, 0, sizeof(wire));
		wire.frame_size_bytes = htonl(enc_info->frame_size_bytes);
		wire.frame_type = enc_info->frame_type;
		wire.qp = enc_info->qp;
		wire.complexity = enc_info->complexity;
		wire.scene_change = enc_info->scene_change;
		wire.gop_state = enc_info->gop_state;
		wire.idr_inserted = enc_info->idr_inserted;
		wire.frames_since_idr = htons(enc_info->frames_since_idr);
		memcpy(buf + off, &wire, sizeof(wire));
		off += sizeof(wire);
	}

	if (transport_info) {
		RtpSidecarTransportInfoWire wire;

		memset(&wire, 0, sizeof(wire));
		wire.fill_pct        = transport_info->fill_pct;
		wire.in_pressure     = transport_info->in_pressure ? 1 : 0;
		wire.throttle_permille =
			htons(transport_info->throttle_permille);
		wire.transport_drops = htonl(transport_info->transport_drops);
		wire.pressure_drops  = htonl(transport_info->pressure_drops);
		wire.packets_sent    = htonl(transport_info->packets_sent);

		flags |= RTP_SIDECAR_FLAG_TRANSPORT_INFO;
		memcpy(buf + off, &wire, sizeof(wire));
		off += sizeof(wire);
	}

	if (attitude_info) {
		RtpSidecarAttitudeWire wire;

		memset(&wire, 0, sizeof(wire));
		wire.roll_cdeg  = (int16_t)htons((uint16_t)attitude_info->roll_cdeg);
		wire.pitch_cdeg = (int16_t)htons((uint16_t)attitude_info->pitch_cdeg);
		wire.yaw_cdeg   = (int16_t)htons((uint16_t)attitude_info->yaw_cdeg);
		wire.status     = htons(attitude_info->status);
		wire.imu_age_ms = htons(attitude_info->imu_age_ms);
		wire.reserved   = 0;

		flags |= RTP_SIDECAR_FLAG_ATTITUDE;
		memcpy(buf + off, &wire, sizeof(wire));
		off += sizeof(wire);
	}

	/* DETECT last (highest bit).  Opaque blob built by the host; drop it
	 * rather than overflow the datagram (a well-formed trailer is bounded
	 * well under RTP_SIDECAR_DGRAM_MAX, so this guard never fires in
	 * practice). */
	if (detect_blob && detect_len && off + detect_len <= sizeof(buf)) {
		flags |= RTP_SIDECAR_FLAG_DETECT;
		memcpy(buf + off, detect_blob, detect_len);
		off += detect_len;
	}

	frame.flags = flags;
	memcpy(buf, &frame, sizeof(frame));

	int rc = 0;
	for (int i = 0; i < RTP_SIDECAR_MAX_SUBS; i++) {
		if (!slot_live_at(&s->subs[i], now))
			continue;
		ssize_t sent = sendto(s->fd, buf, off, MSG_DONTWAIT,
			(struct sockaddr *)&s->subs[i].addr,
			sizeof(s->subs[i].addr));
		if (sent < 0 && errno != EAGAIN) {
			fprintf(stderr, "[sidecar] sendto: %s\n", strerror(errno));
			rc = -1;
		}
	}
	return rc;
}

int rtp_sidecar_send_frame_full(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info,
	const RtpSidecarAttitudeInfo *attitude_info)
{
	return send_frame_impl(s, ssrc, rtp_ts, seq_first, seq_count,
		capture_us, frame_ready_us, enc_info, transport_info,
		attitude_info, NULL, 0);
}

int rtp_sidecar_send_frame_detect(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info,
	const RtpSidecarAttitudeInfo *attitude_info,
	const void *detect_blob, uint16_t detect_len)
{
	return send_frame_impl(s, ssrc, rtp_ts, seq_first, seq_count,
		capture_us, frame_ready_us, enc_info, transport_info,
		attitude_info, detect_blob, detect_len);
}

int rtp_sidecar_send_frame_transport(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info)
{
	return rtp_sidecar_send_frame_full(s, ssrc, rtp_ts,
		seq_first, seq_count, capture_us, frame_ready_us,
		enc_info, transport_info, NULL);
}

int rtp_sidecar_send_frame(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info)
{
	return rtp_sidecar_send_frame_full(s, ssrc, rtp_ts,
		seq_first, seq_count, capture_us, frame_ready_us,
		enc_info, NULL, NULL);
}
