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
#define RTP_SIDECAR_MSG_LINK_STATE  5  /* hub → hub: link/encoder snapshot
                                        * (waybeam-hub mod_link_log, its own
                                        * port — kept here so the message-
                                        * type space has one owner) */

/* Subscription timeout: venc stops sending if no message from probe      */
#define RTP_SIDECAR_SUB_TTL_US    (5 * 1000000ULL)   /* 5 seconds          */

/* Concurrent subscriber slots (protocols/rtp-sidecar.md): the vehicle-
 * local hub attitude consumer, the wfb link_controller and a debug probe
 * must coexist on one port without stealing each other's feed. */
#define RTP_SIDECAR_MAX_SUBS        4

#define RTP_SIDECAR_FLAG_KEYFRAME       0x01
#define RTP_SIDECAR_FLAG_ENC_INFO       0x02
#define RTP_SIDECAR_FLAG_TRANSPORT_INFO 0x04 /* transport stats trailer follows */
#define RTP_SIDECAR_FLAG_ATTITUDE       0x08 /* attitude trailer follows */
#define RTP_SIDECAR_FLAG_DETECT         0x10 /* detection trailer follows (LAST,
                                              * variable-length — see below)   */

/* DETECT trailer (protocols/rtp-sidecar.md).  Variable length, always the last
 * trailer so the fixed ones keep computable offsets.  The host serialises the
 * whole trailer (header + TLV body) and hands the sidecar opaque bytes; the
 * sidecar copies them verbatim and never parses tags. */
#define RTP_SIDECAR_DETECT_MAX        24    /* max objects packed per trailer   */
#define RTP_SIDECAR_DGRAM_MAX         512   /* sender assembly buffer (all
                                             * trailers + max DETECT fit)       */
#define RTP_SIDECAR_DETECT_SCHEMA_V1  1     /* schema_ver: standard BOX tag     */
#define RTP_SIDECAR_DETECT_TRUNCATED  0x0001 /* header.flags bit0               */
#define RTP_SIDECAR_DETECT_TAG_BOX    0x01  /* TLV tag: normalized bbox (10 B)  */
/* model_id registry — a consumer-side class-table selector, allocated in
 * protocols/rtp-sidecar.md.  Keep the two in step: the number is the only
 * thing on the wire that says what `cls` means. */
#define RTP_SIDECAR_DETECT_MODEL_VISDRONE 0 /* VisDrone-10 labels               */
#define RTP_SIDECAR_DETECT_MODEL_PERSON   1 /* single class: "person" (SAR)     */

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
	uint8_t  qp;               /* start QP / closest available per-frame QP.
	                            * CV610 only.  SigmaStar leaves this 0 and
	                            * cannot do better: its encoder fills just
	                            * refType in the H.265 stream-info struct --
	                            * size, every CU count, updAttrCnt and
	                            * startQual all read 0 (device-dumped on
	                            * i6, 0.79.0).  The struct offset is right;
	                            * the driver simply does not populate them.
	                            * A per-frame QP there means parsing
	                            * slice_qp_delta out of the slice header.   */
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
 *   unix://  fill_pct = SIOCOUTQ / learned peer-queue capacity * 100;
 *            capacity is calibrated at the first saturated send and is
 *            estimated from net.unix.max_dgram_qlen before that
 *            transport_drops = unsent datagrams after EAGAIN/ENOBUFS or
 *                              frame-budget exhaustion
 *            packets_sent    = successful sendmsg count
 *   udp://   fill_pct = SIOCOUTQ / SO_SNDBUF * 100; UDP send queues drain
 *            quickly so the signal is noisy; backpressure belongs at the
 *            radio link layer (link_controller) rather than the socket.
 *
 * Forward-compat: probes that don't recognise the flag simply read
 * RtpSidecarFrame (and optionally ENC_INFO) and ignore the trailing
 * bytes.  No version bump required.
 */
typedef struct {
	uint8_t  fill_pct;          /* output queue fill: 0..100              */
	uint8_t  in_pressure;       /* 1 = pressure hysteresis flag asserted  */
	uint8_t  _pad[2];           /* 0.57.0-0.68.x carried throttle_permille
	                             * here; 0.69.0 removed venc's rate clamp,
	                             * so it is reserved again.  Kept as pad
	                             * rather than reclaimed: the trailer must
	                             * stay 16 bytes or every later trailer
	                             * relocates for existing consumers.      */
	uint32_t transport_drops;   /* drops at the transport layer (low 32)  */
	uint32_t pressure_drops;    /* frames the producer observed in
	                             * pressure (was "frames skipped" pre-
	                             * v0.9.2 rollback; ABI name retained)    */
	uint32_t packets_sent;      /* lifetime delivery count (low 32)       */
} RtpSidecarTransportInfoWire; /* 16 bytes */

/**
 * Optional attitude trailer — venc → probe, 12 bytes.
 *
 * Appended after TRANSPORT_INFO (last in flag-bit order, sliding up when
 * lower-bit trailers are absent) when RTP_SIDECAR_FLAG_ATTITUDE is set.
 *
 * Values come from the encoder-local complementary filter fed by the
 * BMI270 sample stream (attitude.enabled, requires imu.enabled).
 * Consumers MUST drop the trailer when status bit0 (valid) is 0.
 * yaw is gyro-integrated and drifts — relative heading only.
 */
#define RTP_SIDECAR_ATT_VALID    0x0001
#define RTP_SIDECAR_ATT_SETTLED  0x0002

typedef struct {
	int16_t  roll_cdeg;        /* 0.1°, right-wing-down positive          */
	int16_t  pitch_cdeg;       /* 0.1°, nose-up positive                  */
	int16_t  yaw_cdeg;         /* 0.1°, relative (integrated, drifts)     */
	uint16_t status;           /* RTP_SIDECAR_ATT_*                       */
	uint16_t imu_age_ms;       /* newest-sample age at capture, saturating */
	uint16_t reserved;         /* 0                                        */
} RtpSidecarAttitudeWire;     /* 12 bytes */

/* Trailers are addressed by walking flag bits and adding sizeof() — a probe
 * finds ATTITUDE by skipping ENC_INFO and TRANSPORT_INFO.  So growing an
 * earlier trailer silently relocates every later one for every existing
 * consumer.  0.57.0 carved TRANSPORT_INFO's _pad[2] into throttle_permille
 * and 0.69.0 returned it to pad, both times keeping the size fixed; pin it so
 * the next such change has to be deliberate. */
_Static_assert(sizeof(RtpSidecarEncInfoWire) == 12,
	"RtpSidecarEncInfoWire must stay 12 bytes (trailer offsets)");
_Static_assert(sizeof(RtpSidecarTransportInfoWire) == 16,
	"RtpSidecarTransportInfoWire must stay 16 bytes (trailer offsets)");
_Static_assert(sizeof(RtpSidecarAttitudeWire) == 12,
	"RtpSidecarAttitudeWire must stay 12 bytes (trailer offsets)");

/**
 * Optional detection trailer header — venc → probe, 16 bytes, followed by a
 * `payload_len`-byte TLV body.  Appended LAST (after ATTITUDE) when
 * RTP_SIDECAR_FLAG_DETECT is set.  The host builds header+body; the sidecar
 * carries it opaquely.  TLV records are `[tag u8][len u8][value…]`; consumers
 * skip unknown tags by len.  See protocols/rtp-sidecar.md.
 */
typedef struct {
	uint16_t model_id;       /* class-table selector (0 = VisDrone-10)     */
	uint16_t schema_ver;     /* RTP_SIDECAR_DETECT_SCHEMA_V1               */
	uint16_t object_count;   /* detections included (≤ RTP_SIDECAR_DETECT_MAX) */
	uint16_t flags;          /* RTP_SIDECAR_DETECT_TRUNCATED               */
	uint32_t detect_seq;     /* monotonic inference id (dedup / freshness) */
	uint16_t payload_len;    /* bytes of TLV body following (= count×12)   */
	uint16_t age_ms;         /* snapshot staleness at frame encode, sat.   */
} RtpSidecarDetectHdr;       /* 16 bytes; TLV body follows */

/**
 * Value of a BOX TLV record (tag RTP_SIDECAR_DETECT_TAG_BOX, len 10) — one
 * detection.  Coords are NORMALIZED 0..65535 of frame W/H, corner form, so the
 * consumer scales by its own decoded resolution with no model geometry needed.
 */
typedef struct {
	uint16_t x1;             /* left   (normalized)                        */
	uint16_t y1;             /* top    (normalized)                        */
	uint16_t x2;             /* right  (normalized)                        */
	uint16_t y2;             /* bottom (normalized)                        */
	uint8_t  score;          /* class probability × 255                    */
	uint8_t  cls;            /* class id (labelled via model_id)           */
} RtpSidecarDetectBoxWire;   /* 10 bytes */

typedef struct {
	RtpSidecarFrame       frame;
	RtpSidecarEncInfoWire enc;
} RtpSidecarFrameExt;         /* 64 bytes */

typedef struct {
	RtpSidecarFrame              frame;
	RtpSidecarEncInfoWire        enc;
	RtpSidecarTransportInfoWire  transport;
} RtpSidecarFrameExtTransport; /* 80 bytes */

typedef struct {
	RtpSidecarFrame              frame;
	RtpSidecarEncInfoWire        enc;
	RtpSidecarTransportInfoWire  transport;
	RtpSidecarAttitudeWire       attitude;
} RtpSidecarFrameExtFull;      /* 92 bytes */

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

/* Host-order attitude snapshot passed to rtp_sidecar_send_frame_full(). */
typedef struct {
	int16_t  roll_cdeg;
	int16_t  pitch_cdeg;
	int16_t  yaw_cdeg;
	uint16_t status;          /* RTP_SIDECAR_ATT_* */
	uint16_t imu_age_ms;
} RtpSidecarAttitudeInfo;

/* ── Sender state (embedded in backend, not used by probe) ───────────── */

typedef struct {
	struct sockaddr_in addr;           /* consumer address                 */
	uint64_t           expires_us;     /* slot expiry (monotonic µs)       */
	                                   /*   0 = slot free                  */
} RtpSidecarSub;

typedef struct {
	int                fd;             /* UDP socket bound to sidecar_port */
	                                   /*   -1 = disabled                  */
	RtpSidecarSub      subs[RTP_SIDECAR_MAX_SUBS]; /* subscriber table:
	                                    * SUBSCRIBE/SYNC_REQ claims or
	                                    * refreshes the matching slot;
	                                    * FRAME fans out to all live slots
	                                    * (protocols/rtp-sidecar.md) */
	uint64_t           frame_id;       /* monotonic frame counter          */
} RtpSidecarSender;

/* True iff the sidecar socket is open AND a probe is currently subscribed.
 * Producer hot path uses this to gate transport-pressure observation —
 * the only consumer of in_pressure / pressure_drops / fill_pct is the
 * trailer, so observation is dead work when no one is listening. */
int rtp_sidecar_is_subscribed(const RtpSidecarSender *s);

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

/**
 * Same as rtp_sidecar_send_frame_transport but optionally appends the
 * attitude trailer (RTP_SIDECAR_FLAG_ATTITUDE) after TRANSPORT_INFO.
 * attitude_info == NULL → identical to send_frame_transport.
 */
int rtp_sidecar_send_frame_full(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info,
	const RtpSidecarAttitudeInfo *attitude_info);

/**
 * Same as rtp_sidecar_send_frame_full but optionally appends the opaque DETECT
 * trailer (RTP_SIDECAR_FLAG_DETECT) after ATTITUDE.  detect_blob points at a
 * fully serialised trailer (RtpSidecarDetectHdr + TLV body, network byte order)
 * that the caller built — the sidecar copies it verbatim and never parses tags.
 * detect_blob == NULL or detect_len == 0 → identical to send_frame_full.  The
 * blob is silently dropped (flag left clear) if it would overflow the datagram
 * assembly buffer (RTP_SIDECAR_DGRAM_MAX); the caller bounds it so this cannot
 * happen for a well-formed trailer.
 */
int rtp_sidecar_send_frame_detect(RtpSidecarSender *s,
	uint32_t ssrc, uint32_t rtp_ts,
	uint16_t seq_first, uint16_t seq_count,
	uint64_t capture_us, uint64_t frame_ready_us,
	const RtpSidecarEncInfo *enc_info,
	const RtpSidecarTransportInfo *transport_info,
	const RtpSidecarAttitudeInfo *attitude_info,
	const void *detect_blob, uint16_t detect_len);

#endif /* RTP_SIDECAR_H */
