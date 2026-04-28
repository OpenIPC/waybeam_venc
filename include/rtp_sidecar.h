#ifndef RTP_SIDECAR_H
#define RTP_SIDECAR_H

#include <stdint.h>
#include <netinet/in.h>

/* ── Wire protocol constants ─────────────────────────────────────────── */

#define RTP_SIDECAR_MAGIC         0x52545053u  /* "RTPS" big-endian        */
#define RTP_SIDECAR_VERSION       1

/*
 * Message types
 *
 * Flow:
 *   venc binds to sidecar_port and LISTENS.  Channel is silent until the
 *   probe (ground station) sends MSG_SUBSCRIBE.
 *
 *   probe → venc  MSG_SUBSCRIBE    "start sending me frame metadata"
 *   venc  → probe MSG_FRAME        one per encoded frame (while subscribed)
 *   probe → venc  MSG_SYNC_REQ     NTP-style clock-offset probe
 *   venc  → probe MSG_SYNC_RESP    echo t1, add t2/t3
 *
 *   Subscription expires if venc does not receive MSG_SUBSCRIBE or
 *   MSG_SYNC_REQ from the subscriber within RTP_SIDECAR_SUB_TTL_US.
 *   Both message types refresh the expiry timer.
 *
 *   The probe's recvfrom source address is used as the reply destination;
 *   no IP/port configuration is needed on the venc side.
 */
#define RTP_SIDECAR_MSG_SUBSCRIBE   1  /* probe → venc: start/refresh sub   */
#define RTP_SIDECAR_MSG_FRAME       2  /* venc → probe: frame metadata       */
#define RTP_SIDECAR_MSG_SYNC_REQ    3  /* probe → venc: clock sync request   */
#define RTP_SIDECAR_MSG_SYNC_RESP   4  /* venc → probe: clock sync response  */

/* Subscription timeout: venc stops sending if no message from probe      */
#define RTP_SIDECAR_SUB_TTL_US    (5 * 1000000ULL)   /* 5 seconds          */

#define RTP_SIDECAR_FLAG_KEYFRAME       0x01
#define RTP_SIDECAR_FLAG_ENC_INFO       0x02
#define RTP_SIDECAR_FLAG_TRANSPORT_INFO 0x04 /* transport stats trailer follows */

/*
 * Frame type values carried in the optional encoder-feedback trailer.
 * These stay local to the sidecar ABI.
 */
#define RTP_SIDECAR_FRAME_P       0
#define RTP_SIDECAR_FRAME_I       1
#define RTP_SIDECAR_FRAME_IDR     2

/* ── Wire structs (all fields network byte order) ────────────────────── */

#pragma pack(push, 1)

/**
 * Subscribe — probe → venc, 8 bytes.
 * Sent by the probe to start receiving frame metadata.
 * Also serves as a keepalive; resend every ~2 s to prevent expiry.
 */
typedef struct {
	uint32_t magic;          /* RTP_SIDECAR_MAGIC                          */
	uint8_t  version;        /* RTP_SIDECAR_VERSION                        */
	uint8_t  msg_type;       /* RTP_SIDECAR_MSG_SUBSCRIBE                  */
	uint8_t  _pad[2];
} RtpSidecarSubscribe;       /* 8 bytes */

/**
 * Frame metadata — venc → probe, 52 bytes, one per encoded frame.
 *
 * Sent immediately AFTER the last RTP packet of the frame has been handed
 * to the kernel.  This single message brackets the complete sender-side
 * path:
 *
 *   capture_us → [encode] → frame_ready_us → [packetise+send] → last_pkt_send_us
 *                                                                ↕ (network)
 *                                                          recv_last_us (probe)
 *
 * Receiver matches by (ssrc, rtp_timestamp).
 */
typedef struct {
	uint32_t magic;          /* RTP_SIDECAR_MAGIC                          */
	uint8_t  version;        /* RTP_SIDECAR_VERSION                        */
	uint8_t  msg_type;       /* RTP_SIDECAR_MSG_FRAME                      */
	uint8_t  stream_id;      /* 0 = video (reserved for future use)        */
	uint8_t  flags;          /* RTP_SIDECAR_FLAG_*                         */
	uint32_t ssrc;           /* matches RTP SSRC for this session          */
	uint32_t rtp_timestamp;  /* matches RTP timestamp for this frame       */
	uint64_t frame_id;       /* monotonic sender frame counter (0-based)   */
	uint64_t frame_ready_us; /* CLOCK_MONOTONIC_RAW µs at encode-complete  */
	uint16_t seq_first;      /* RTP seq of first packet this frame         */
	uint16_t seq_count;      /* number of RTP packets in this frame        */
	uint64_t capture_us;     /* encoder PTS converted to CLOCK_MONOTONIC µs
	                          * (earliest available timestamp on the sender)
	                          * 0 = not available                           */
	uint64_t last_pkt_send_us; /* CLOCK_MONOTONIC_RAW µs after final sendmsg */
} RtpSidecarFrame;           /* 52 bytes */

/**
 * Optional encoder-feedback trailer — venc → probe, 12 bytes.
 *
 * Appended immediately after RtpSidecarFrame when
 * RTP_SIDECAR_FLAG_ENC_INFO is set.
 *
 * Values come from the inline scene detector's per-frame telemetry.
 */
typedef struct {
	uint32_t frame_size_bytes; /* encoded frame bytes                        */
	uint8_t  frame_type;       /* RTP_SIDECAR_FRAME_*                        */
	uint8_t  qp;               /* start QP / closest available per-frame QP  */
	uint8_t  complexity;       /* 0-255 scene complexity estimate            */
	uint8_t  scene_change;     /* 1 = scene spike detected                   */
	uint8_t  gop_state;        /* GopState enum value                        */
	uint8_t  idr_inserted;     /* 1 = controller requested IDR after frame   */
	uint16_t frames_since_idr; /* controller frames-since-IDR counter        */
} RtpSidecarEncInfoWire;     /* 12 bytes */

/**
 * Optional transport-stats trailer — venc → probe, 16 bytes.
 *
 * Appended after the ENC_INFO trailer (or directly after RtpSidecarFrame
 * when ENC_INFO is absent) when RTP_SIDECAR_FLAG_TRANSPORT_INFO is set
 * in RtpSidecarFrame.flags.
 *
 * Carries producer-local output observability that's meaningful for any
 * transport with a queueing model (SHM ring, UNIX datagram socket, UDP
 * socket): output queue fill, backpressure hysteresis state, lifetime
 * delivery stats.  link_controller and other adaptive controllers can
 * react without an extra HTTP roundtrip.
 *
 * Field semantics by transport:
 *   shm://   fill_pct = (write-read)/slot_count*100
 *            transport_drops = ring full → packet dropped
 *            packets_sent    = ring writes
 *   unix://  fill_pct = SIOCOUTQ / SO_SNDBUF * 100
 *            transport_drops = sendmsg(EAGAIN) | ENOBUFS count
 *            packets_sent    = successful sendmsg count
 *   udp://   same as unix:// but UDP send queues drain quickly so the
 *            signal is noisy; backpressure typically belongs at the
 *            radio link layer (link_controller) rather than the socket.
 *
 * Forward-compat: probes that don't recognise the flag simply read
 * RtpSidecarFrame (and optionally ENC_INFO) and ignore the trailing
 * bytes.  No version bump required.
 */
typedef struct {
	uint8_t  fill_pct;          /* output queue fill: 0..100              */
	uint8_t  in_pressure;       /* 1 = pressure hysteresis flag asserted  */
	uint8_t  _pad[2];           /* reserved, must be 0; future flags      */
	uint32_t transport_drops;   /* drops at the transport layer (low 32)  */
	uint32_t pressure_drops;    /* frames the producer observed in
	                             * pressure (was "frames skipped" pre-
	                             * v0.9.2 rollback; ABI name retained)    */
	uint32_t packets_sent;      /* lifetime delivery count (low 32)       */
} RtpSidecarTransportInfoWire; /* 16 bytes */

typedef struct {
	RtpSidecarFrame       frame;
	RtpSidecarEncInfoWire enc;
} RtpSidecarFrameExt;         /* 64 bytes */

typedef struct {
	RtpSidecarFrame              frame;
	RtpSidecarEncInfoWire        enc;
	RtpSidecarTransportInfoWire  transport;
} RtpSidecarFrameExtTransport; /* 80 bytes */

/** Clock sync request — probe → venc, 16 bytes */
typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  msg_type;       /* RTP_SIDECAR_MSG_SYNC_REQ                   */
	uint8_t  _pad[2];
	uint64_t t1_us;          /* probe's monotonic clock at send (µs)       */
} RtpSidecarSyncReq;         /* 16 bytes */

/** Clock sync response — venc → probe, 32 bytes */
typedef struct {
	uint32_t magic;
	uint8_t  version;
	uint8_t  msg_type;       /* RTP_SIDECAR_MSG_SYNC_RESP                  */
	uint8_t  _pad[2];
	uint64_t t1_us;          /* echo of req.t1_us                          */
	uint64_t t2_us;          /* venc monotonic clock at recv (µs)          */
	uint64_t t3_us;          /* venc monotonic clock at reply send (µs)    */
} RtpSidecarSyncResp;        /* 32 bytes */

#pragma pack(pop)

/* Host-order encoder feedback passed to rtp_sidecar_send_frame(). */
typedef struct {
	uint32_t frame_size_bytes;
	uint8_t  frame_type;
	uint8_t  qp;
	uint8_t  complexity;
	uint8_t  scene_change;
	uint8_t  gop_state;
	uint8_t  idr_inserted;
	uint16_t frames_since_idr;
} RtpSidecarEncInfo;

/* Host-order transport snapshot passed to rtp_sidecar_send_frame_transport(). */
typedef struct {
	uint8_t  fill_pct;
	uint8_t  in_pressure;
	uint32_t transport_drops;
	uint32_t pressure_drops;
	uint32_t packets_sent;
} RtpSidecarTransportInfo;

/* ── Sender state (embedded in backend, not used by probe) ───────────── */

typedef struct {
	int                fd;              /* UDP socket bound to sidecar_port */
	                                    /*   -1 = disabled                  */
	struct sockaddr_in subscriber;      /* active probe address             */
	uint64_t           sub_expires_us; /* subscriber expiry (monotonic µs) */
	                                    /*   0 = no subscriber              */
	uint64_t           frame_id;       /* monotonic frame counter          */
} RtpSidecarSender;

/**
 * Initialise the sender.
 *
 * Binds a UDP socket to sidecar_port on INADDR_ANY.  No outbound destination
 * is configured here — the probe's address is learned from the first
 * MSG_SUBSCRIBE packet received via rtp_sidecar_poll().
 *
 * sidecar_port == 0 → disabled (fd = -1), returns 0.
 * Returns 0 on success or disabled, -1 on socket error.
 */
int rtp_sidecar_sender_init(RtpSidecarSender *s, uint16_t sidecar_port);

/** Release socket.  Safe to call on a disabled sender. */
void rtp_sidecar_sender_close(RtpSidecarSender *s);

/**
 * Poll for incoming probe packets (MSG_SUBSCRIBE, MSG_SYNC_REQ).
 *
 * Uses MSG_DONTWAIT — never blocks.  Call once per frame (or more often).
 *
 *   MSG_SUBSCRIBE  — records/refreshes the subscriber address and TTL.
 *   MSG_SYNC_REQ   — replies with MSG_SYNC_RESP and refreshes subscriber.
 *
 * Safe to call on a disabled sender (no-op).
 */
void rtp_sidecar_poll(RtpSidecarSender *s);

/**
 * Send one frame-metadata packet to the active subscriber (MSG_FRAME).
 *
 * Call immediately AFTER the last RTP packet of the frame has been handed
 * to the kernel via sendmsg/sendto.  Stamps last_pkt_send_us internally.
 *
 * ssrc / rtp_ts  : RTP identifiers for this frame.
 * seq_first      : RTP seq of the first packet in this frame.
 * seq_count      : number of RTP packets sent for this frame.
 * capture_us     : encoder PTS converted to CLOCK_MONOTONIC µs, or 0.
 * frame_ready_us : CLOCK_MONOTONIC µs captured before RTP sending began.
 * enc_info       : optional host-order encoder feedback trailer.
 *
 * Returns 0 (success, no subscriber, or disabled), -1 on send error.
 */
int rtp_sidecar_send_frame(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info);

/**
 * Same as rtp_sidecar_send_frame but optionally appends a transport
 * stats trailer.  If transport_info is non-NULL,
 * RTP_SIDECAR_FLAG_TRANSPORT_INFO is set in the frame flags and the
 * trailer follows ENC_INFO (or directly follows the base frame when
 * enc_info is NULL).  Old probes that don't recognise the flag read
 * the base frame (and ENC_INFO if present) and ignore the trailing
 * bytes.
 */
int rtp_sidecar_send_frame_transport(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info);

#endif /* RTP_SIDECAR_H */
