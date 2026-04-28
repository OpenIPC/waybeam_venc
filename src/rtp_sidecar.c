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

/* ── Subscriber helpers ──────────────────────────────────────────────── */

static int sub_active_at(const RtpSidecarSender *s, uint64_t now)
{
	return s->sub_expires_us != 0 && now < s->sub_expires_us;
}

static void sub_refresh_at(RtpSidecarSender *s, const struct sockaddr_in *src,
	uint64_t now)
{
	s->subscriber    = *src;
	s->sub_expires_us = now + RTP_SIDECAR_SUB_TTL_US;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int rtp_sidecar_sender_init(RtpSidecarSender *s, uint16_t sidecar_port)
{
	if (!s)
		return -1;

	s->fd             = -1;
	s->frame_id       = 0;
	s->sub_expires_us = 0;
	memset(&s->subscriber, 0, sizeof(s->subscriber));

	if (sidecar_port == 0)
		return 0;  /* disabled — not an error */

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
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
	if (!s || s->fd < 0)
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
			if (!sub_active_at(s, now))
				fprintf(stderr, "[sidecar] probe subscribed from %s:%u\n",
				        inet_ntoa(src.sin_addr), ntohs(src.sin_port));
			sub_refresh_at(s, &src, now);
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

int rtp_sidecar_send_frame_transport(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info)
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

	RtpSidecarFrameExtTransport msg;
	size_t msg_len = sizeof(msg.frame);

	memset(&msg, 0, sizeof(msg));
	msg.frame.magic            = htonl(RTP_SIDECAR_MAGIC);
	msg.frame.version          = RTP_SIDECAR_VERSION;
	msg.frame.msg_type         = RTP_SIDECAR_MSG_FRAME;
	msg.frame.stream_id        = 0;
	msg.frame.flags            = 0;
	msg.frame.ssrc             = htonl(ssrc);
	msg.frame.rtp_timestamp    = htonl(rtp_ts);
	msg.frame.frame_id         = sidecar_htobe64(s->frame_id++);
	msg.frame.frame_ready_us   = sidecar_htobe64(frame_ready_us);
	msg.frame.seq_first        = htons(seq_first);
	msg.frame.seq_count        = htons(seq_count);
	msg.frame.capture_us       = sidecar_htobe64(capture_us);
	msg.frame.last_pkt_send_us = sidecar_htobe64(now);

	if (enc_info) {
		msg.frame.flags |= RTP_SIDECAR_FLAG_ENC_INFO;
		if (enc_info->frame_type == RTP_SIDECAR_FRAME_I ||
		    enc_info->frame_type == RTP_SIDECAR_FRAME_IDR)
			msg.frame.flags |= RTP_SIDECAR_FLAG_KEYFRAME;

		msg.enc.frame_size_bytes = htonl(enc_info->frame_size_bytes);
		msg.enc.frame_type = enc_info->frame_type;
		msg.enc.qp = enc_info->qp;
		msg.enc.complexity = enc_info->complexity;
		msg.enc.scene_change = enc_info->scene_change;
		msg.enc.gop_state = enc_info->gop_state;
		msg.enc.idr_inserted = enc_info->idr_inserted;
		msg.enc.frames_since_idr = htons(enc_info->frames_since_idr);
		msg_len = offsetof(RtpSidecarFrameExtTransport, transport);
	}

	if (transport_info) {
		/* Trailer follows ENC_INFO when present, or sits directly
		 * after the base frame otherwise.  We always serialise to
		 * the "after enc" slot in the contiguous struct; when enc is
		 * absent we slide the trailer up so old probes that read
		 * just the base frame don't see partial enc bytes between
		 * frame and transport. */
		RtpSidecarTransportInfoWire wire;
		size_t trailer_offset;

		memset(&wire, 0, sizeof(wire));
		wire.fill_pct        = transport_info->fill_pct;
		wire.in_pressure     = transport_info->in_pressure ? 1 : 0;
		wire.transport_drops = htonl(transport_info->transport_drops);
		wire.pressure_drops  = htonl(transport_info->pressure_drops);
		wire.packets_sent    = htonl(transport_info->packets_sent);

		msg.frame.flags |= RTP_SIDECAR_FLAG_TRANSPORT_INFO;
		trailer_offset = enc_info
			? offsetof(RtpSidecarFrameExtTransport, transport)
			: sizeof(RtpSidecarFrame);
		memcpy((uint8_t *)&msg + trailer_offset, &wire, sizeof(wire));
		msg_len = trailer_offset + sizeof(wire);
	}

	ssize_t sent = sendto(s->fd, &msg, msg_len, MSG_DONTWAIT,
		(struct sockaddr *)&s->subscriber,
		sizeof(s->subscriber));
	if (sent < 0 && errno != EAGAIN) {
		fprintf(stderr, "[sidecar] sendto: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int rtp_sidecar_send_frame(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info)
{
	return rtp_sidecar_send_frame_transport(s, ssrc, rtp_ts,
		seq_first, seq_count, capture_us, frame_ready_us,
		enc_info, NULL);
}
