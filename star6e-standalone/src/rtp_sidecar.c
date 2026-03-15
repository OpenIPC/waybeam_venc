#include "rtp_sidecar.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

/* ── Clock helper ────────────────────────────────────────────────────── */

static uint64_t now_us(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ── Subscriber helpers ──────────────────────────────────────────────── */

static int sub_active(const RtpSidecarSender *s)
{
	return s->sub_expires_us != 0 && now_us() < s->sub_expires_us;
}

static void sub_refresh(RtpSidecarSender *s, const struct sockaddr_in *src)
{
	s->subscriber    = *src;
	s->sub_expires_us = now_us() + RTP_SIDECAR_SUB_TTL_US;
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

		switch (msg_type) {

		case RTP_SIDECAR_MSG_SUBSCRIBE:
			/* Any subscribe (including keepalive) refreshes the TTL */
			if (!sub_active(s))
				fprintf(stderr, "[sidecar] probe subscribed from %s:%u\n",
				        inet_ntoa(src.sin_addr), ntohs(src.sin_port));
			sub_refresh(s, &src);
			break;

		case RTP_SIDECAR_MSG_SYNC_REQ:
			if ((size_t)n < sizeof(RtpSidecarSyncReq))
				continue;
			{
				const RtpSidecarSyncReq *req = (const RtpSidecarSyncReq *)buf;
				uint64_t t2 = now_us();

				RtpSidecarSyncResp resp;
				resp.magic    = htonl(RTP_SIDECAR_MAGIC);
				resp.version  = RTP_SIDECAR_VERSION;
				resp.msg_type = RTP_SIDECAR_MSG_SYNC_RESP;
				resp._pad[0]  = 0;
				resp._pad[1]  = 0;
				resp.t1_us    = req->t1_us;  /* echo verbatim */
				resp.t2_us    = sidecar_htobe64(t2);
				resp.t3_us    = sidecar_htobe64(now_us());

				sendto(s->fd, &resp, sizeof(resp), MSG_DONTWAIT,
				       (struct sockaddr *)&src, src_len);
				/* Also refresh subscriber so sync acts as keepalive */
				sub_refresh(s, &src);
			}
			break;

		default:
			break;
		}
	}
}

/* ── Frame metadata sender ───────────────────────────────────────────── */

int rtp_sidecar_send_frame(RtpSidecarSender *s,
                           uint32_t ssrc, uint32_t rtp_ts,
                           uint16_t seq_first, uint16_t seq_count,
                           uint64_t capture_us, uint64_t frame_ready_us)
{
	if (!s || s->fd < 0)
		return 0;  /* disabled */
	if (!sub_active(s))
		return 0;  /* no subscriber — channel stays silent */

	RtpSidecarFrame f;
	f.magic            = htonl(RTP_SIDECAR_MAGIC);
	f.version          = RTP_SIDECAR_VERSION;
	f.msg_type         = RTP_SIDECAR_MSG_FRAME;
	f.stream_id        = 0;
	f.flags            = 0;
	f.ssrc             = htonl(ssrc);
	f.rtp_timestamp    = htonl(rtp_ts);
	f.frame_id         = sidecar_htobe64(s->frame_id++);
	f.frame_ready_us   = sidecar_htobe64(frame_ready_us);
	f.seq_first        = htons(seq_first);
	f.seq_count        = htons(seq_count);
	f.capture_us       = sidecar_htobe64(capture_us);
	f.last_pkt_send_us = sidecar_htobe64(now_us());

	ssize_t sent = sendto(s->fd, &f, sizeof(f), MSG_DONTWAIT,
	                      (struct sockaddr *)&s->subscriber,
	                      sizeof(s->subscriber));
	if (sent < 0 && errno != EAGAIN) {
		fprintf(stderr, "[sidecar] sendto: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}
