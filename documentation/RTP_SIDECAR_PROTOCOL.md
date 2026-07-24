# RTP Sidecar Protocol

Out-of-band per-frame metadata channel that rides alongside the RTP video
stream. `waybeam_venc` is the **producer**; a same-network consumer (viewer,
ground station) subscribes and receives one datagram per encoded frame.

The canonical wire definition is `include/rtp_sidecar.h`; this document is the
human-readable companion. Field offsets and constants here must track that
header.

## Transport & subscription

- UDP. venc binds `sidecar_port` and **listens** — the channel is silent until a
  consumer subscribes, and stops when the subscription lapses.
- The consumer sends `MSG_SUBSCRIBE (1)`; venc replies with `MSG_FRAME (2)`
  datagrams while the subscription is live (refresh before `RTP_SIDECAR_SUB_TTL`).
- Clock sync: consumer `MSG_SYNC_REQ (3)` → venc `MSG_SYNC_RESP (4)`.
- Multi-subscriber: one `sendto` per frame **per subscriber**.
- Magic `0x52545053` ("RTPS", big-endian), version `1`.

Receive buffers must hold **≥512 B** (base 52 + all trailers with a full DETECT
body = 396 B today; 512 leaves headroom).

## FRAME layout (52-byte base)

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `0x52545053` |
| 4 | 1 | version | `1` |
| 5 | 1 | msg_type | `2` (FRAME) |
| 6 | 1 | stream_id | `0` = video |
| **7** | **1** | **flags** | flags live at **byte 7**, not byte 5 (byte 5 is msg_type — a common probe-writing trap) |
| 8 | 4 | ssrc | matches RTP SSRC |
| 12 | 4 | rtp_timestamp | matches the frame's RTP timestamp |
| 16 | 8 | frame_id | monotonic sender counter |
| 24 | 8 | frame_ready_us | `CLOCK_MONOTONIC_RAW` µs at encode-complete |
| 32 | 2 | seq_first | RTP seq of the frame's first packet |
| 34 | 2 | seq_count | RTP packets in this frame |
| 36 | 8 | capture_us | encoder PTS as `CLOCK_MONOTONIC` µs, `0` = n/a |
| 44 | 8 | last_pkt_send_us | µs after the final `sendmsg` |

## Flags and optional trailers

| Flag | Trailer | Size |
|---|---|---|
| 0x01 KEYFRAME | none | — |
| 0x02 ENC_INFO | encoder feedback | 12 B |
| 0x04 TRANSPORT_INFO | output-queue stats | 16 B |
| 0x08 ATTITUDE | vehicle attitude | 12 B |
| 0x10 DETECT | object detections (variable) | 16 B + N×12 B |

Trailers are appended **in flag-bit order** directly after byte 52, each sliding
up when a lower-bit trailer is absent. Consumers MUST compute offsets by walking
the flags in order with length guards, and MUST ignore unknown flag bits and any
trailing bytes — this is what makes new trailers non-breaking. DETECT is the only
variable-length trailer and is always **last** (highest bit), so the fixed
trailers keep computable offsets.

**ENC_INFO (0x02), 12 B**: `frame_size_bytes u32`, `frame_type u8`
(0=P 1=I 2=IDR), `qp u8`, `complexity u8`, `scene_change u8`, `gop_state u8`,
`idr_inserted u8`, `frames_since_idr u16`.

**TRANSPORT_INFO (0x04), 16 B**: `fill_pct u8`, `in_pressure u8`, `_pad u8[2]`,
`transport_drops u32`, `pressure_drops u32`, `packets_sent u32`.

**ATTITUDE (0x08), 12 B** — encoder-local complementary-filter attitude, frame
-correlated via `frame_id`; enabled by `imu.attitude` (requires `imu.enabled`):

| Off | Size | Field | Notes |
|---|---|---|---|
| +0 | 2 | roll_cdeg | int16, 0.1° units, right-wing-down positive |
| +2 | 2 | pitch_cdeg | int16, 0.1° units, nose-up positive |
| +4 | 2 | yaw_cdeg | int16, 0.1° units; gyro-integrated, relative heading only |
| +6 | 2 | status | bit0 = valid, bit1 = estimator settled; rest reserved 0 |
| +8 | 2 | imu_age_ms | u16, age of newest IMU sample at frame capture, saturating |
| +10 | 2 | reserved | 0 |

Consumers must drop the trailer when `status.valid` is 0.

## DETECT trailer (0x10) — NPU object detection

16-byte header + N×12 B TLV body. Star6E only; enabled by `detect.enabled`.
Detections ride the FRAME so they are frame-correlated via `frame_id` and add no
extra packets. Appended **last** (after ATTITUDE); the only variable-length
trailer.

Header (16 B):

| Off | Size | Field | Notes |
|---|---|---|---|
| +0 | 2 | model_id | class-table selector; `0` = VisDrone-10. Configured via `detect.modelId` — the plugin cannot infer it |
| +2 | 2 | schema_ver | wire schema; `1` = standard BOX tag |
| +4 | 2 | object_count | detections actually included (≤ 24) |
| +6 | 2 | flags | bit0 = TRUNCATED (objects dropped to fit the MTU budget) |
| +8 | 4 | detect_seq | monotonic inference id — dedup / freshness |
| +12 | 2 | payload_len | bytes of TLV body that follow (= count×12) |
| +14 | 2 | age_ms | staleness of this snapshot at frame encode, saturating |

TLV body: a sequence of `[tag u8][len u8][value…]` records, network byte order.
Consumers skip unknown tags by `len`. One **public standard tag** is defined:

- **`0x01` BOX**, `len = 10`: `x1,y1,x2,y2` (u16 each, **normalized 0..65535** of
  frame W/H, corner form), `score` (u8, prob×255), `cls` (u8, mapped to a label
  via `model_id`).

Coordinates are normalized, not model-pixel: the detection tap scales the full
frame into the model input **linearly per-axis**, so a normalized coordinate is
identical in model space and display space — the consumer multiplies by its own
decoded W/H, with no model geometry on the wire. Tags `0x80–0xFF` are **reserved
for private, plugin-defined records** (keypoints, distance, classifier vectors);
a consumer that does not recognize a tag skips it, so private models extend the
payload without a spec change.

### model_id registry

`cls` is meaningless without knowing which model produced it, and the wire
carries no model metadata beyond this number. Producer and consumer must agree
out of band. A consumer should treat an unknown `model_id` as "unlabelled class
N" rather than falling back to table 0 (a silent mismatch relabels every box).

| id | class table | classes |
|---|---|---|
| 0 | VisDrone-10 | pedestrian, people, bicycle, car, van, truck, tricycle, awning-tricycle, bus, motor |
| 1 | SAR person | person |

### Emission semantics

The producer emits the latest detection snapshot on **every** FRAME while a
subscriber is live (not only when `detect_seq` advances) — on a lossy RF video
link the repeat is cheap and loss-resilient, and `detect_seq`/`age_ms` let the
consumer dedup and fade stale boxes. The trailer is **omitted** (flag clear) when
no valid detection exists yet; `object_count = 0` means "ran, empty scene". The
host fills BOX tags highest-score-first until the datagram MTU budget is reached,
then sets TRUNCATED — still exactly one `sendto` per frame per subscriber. Model
semantics live in the detector plugin (see the detect plugin boundary) and in the
consumer's `model_id` table.

## Compatibility

All FRAME consumers must be tolerant of appended unknown-flag trailers: walk the
flags in order with length guards and ignore unknown flag bits plus trailing
bytes. Adding DETECT (0x10) last is non-breaking on that basis — it is flag-gated
and older consumers ignore it.
