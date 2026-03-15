# WFB-NG TX Integration — Feasibility Study

## 1. Executive Summary

This study evaluates integrating wfb-ng's transmitter (`tx.cpp`) as a third
output mode alongside the existing RTP and Compact UDP sinks.  The goal is
to feed the existing RTP stream directly from venc into wfb-ng's
FEC + encryption + raw-socket wifi injection pipeline — removing the UDP
socket entirely and achieving as close to zero-copy as possible, while keeping
the wfb-ng code trivially mergeable with upstream updates.

**Verdict: feasible**, with a clean separation achievable through a thin C
adapter layer that calls into wfb-ng's `Transmitter::send_packet()` API.
The existing RTP encapsulation (header build, H.265 FU fragmentation,
VPS/SPS/PPS prepend) is fully reused — only the final `sendmsg()` call is
replaced with `wfb_tx_send()`.  The recommended integration strategy is a
**git-subtree + minimal patch** approach.

---

## 2. Architecture Overview

### Current venc RTP output pipeline

```
MI_VENC_GetStream()                ← SDK returns MI_VENC_Stream_t
    │
    ▼
send_frame_rtp()                   ← iterate packs, extract NALs
    │  ├── strip start codes, cache VPS/SPS/PPS
    │  ├── prepend VPS/SPS/PPS before IDR frames
    │  └── for each NAL:
    │       └── send_nal_rtp_hevc()
    │            ├── small NAL (≤1200): send_rtp_packet() as single packet
    │            └── large NAL (>1200): send_fu_hevc() fragmentation
    │
    ▼
send_rtp_packet()                  ← builds 12-byte RTP header + payload
    │  ├── iovec[0] = RTP header (12 bytes, built on stack)
    │  ├── iovec[1] = payload (points directly into SDK DMA buffer)
    │  └── sendmsg(socket_handle, &msg, 0)  ← UDP socket
    │
    ▼
Network (UDP to ground station)
```

Key insight: `send_rtp_packet()` (backend_star6e.c:506) is the **single
choke point** where all RTP data exits to the network.  It uses `sendmsg()`
with an iovec that already points at the SDK DMA buffer — zero userspace
copies up to this point.

### wfb-ng TX pipeline

```
Input (UDP/Unix socket)
    │
    ▼
Transmitter::send_packet(buf, size, flags)
    │  ├── memcpy into FEC block[fragment_idx]
    │  └── prepend wpacket_hdr_t (flags + size)
    │
    ▼  (when fragment_idx == fec_k)
fec_encode_simd()            ← generate parity fragments
    │
    ▼
send_block_fragment()
    │  ├── ChaCha20-Poly1305 AEAD encrypt
    │  ├── prepend wblock_hdr_t + IEEE 802.11 + radiotap
    │  └── inject_packet() → raw socket sendmsg()
    │
    ▼
Wireless NIC (monitor mode)
```

### Proposed integrated pipeline

The core idea: **reuse the entire existing RTP pipeline unchanged** and
replace only the final `sendmsg()` in `send_rtp_packet()` with a call to
`wfb_tx_send()`.  The RTP header + payload are assembled into a flat buffer
(or scatter-gathered) and passed directly to wfb-ng as a single packet.

```
MI_VENC_GetStream()
    │
    ▼
send_frame_rtp()                   ← UNCHANGED: same NAL iteration,
    │                                 same FU fragmentation, same VPS/SPS/PPS
    │
    ▼
send_rtp_packet()                  ← MODIFIED: mode-aware send
    │  ├── builds 12-byte RTP header (same as today)
    │  │
    │  ├── [UDP/RTP mode]: sendmsg() with iovec → UDP socket
    │  │                   (existing path, zero copies)
    │  │
    │  └── [WFB mode]: assemble header+payload into flat buffer
    │       └── wfb_tx_send(rtp_packet, total_len)
    │            ├── memcpy into FEC block slot (1 copy, SIMD-aligned)
    │            └── trigger FEC when block full
    │
    ▼
[FEC + encrypt + inject — unchanged wfb-ng code]
    │
    ▼
Wireless NIC (monitor mode)
```

**Why this is better than a separate `send_frame_wfb()` function:**
- The RTP stream is preserved end-to-end.  The wfb-ng RX peer delivers
  standard RTP packets to the ground station's video decoder — no custom
  framing needed on the receiver side.
- No code duplication: NAL iteration, start code stripping, FU
  fragmentation, VPS/SPS/PPS injection are all reused.
- The diff is minimal: `send_rtp_packet()` gains one mode branch.
- `send_frame_compact()` can also be routed through WFB the same way if
  needed in the future.

---

## 3. Zero-Copy Analysis

### Where copies happen today (RTP/UDP path)

| Step | Copy? | Notes |
|------|-------|-------|
| SDK DMA buf → RTP header build | **No** | iovec[1] points at SDK buffer |
| RTP header (12 bytes, stack) | **No** | iovec[0], trivial |
| sendmsg iovec → kernel skb | **Yes** | kernel copies to skb (unavoidable) |

Total: **0 userspace copies** — iovec scatter-gather avoids any memcpy.

### Where copies happen with WFB integration

| Step | Copy? | Avoidable? | Notes |
|------|-------|------------|-------|
| SDK DMA buf → RTP header build | **No** | — | Same iovec assembly as today |
| RTP header + payload → flat buffer | **Yes** | **Partially** | Need contiguous buffer for `wfb_tx_send()`. The 12-byte RTP header is on the stack, payload points at SDK DMA. Must combine into one flat buffer. For small NALs (≤1200 bytes, single RTP packet) this is one ~1.2 KB memcpy. |
| Flat RTP packet → FEC block slot | **Yes** | **No** | FEC encoder needs all k fragments in aligned, contiguous SIMD buffers. SDK buffer lifetime ends at `ReleaseStream()`, but FEC needs all k fragments simultaneously. |
| FEC block → encrypt | **No** | — | In-place on block buffer |
| Encrypted buf → raw socket iovec | **No** | — | Scatter-gather |
| Kernel → NIC TX ring | **Yes** | **No** | Raw socket still goes through skb |

Total: **1–2 userspace copies** per RTP packet (flatten + FEC block).

### Optimization: collapse to 1 copy

The flatten step can be **merged with the FEC block copy** by writing the
RTP header + payload directly into the FEC block slot:

```c
/* In send_rtp_packet(), WFB mode path: */
uint8_t *fec_slot = wfb_tx_get_block_ptr(tx, &slot_size);
memcpy(fec_slot, rtp_header, 12);           /* 12 bytes */
memcpy(fec_slot + 12, payload, payload_len); /* SDK DMA buf */
wfb_tx_commit_block_fragment(tx, 12 + payload_len, flags);
```

This writes directly into the pre-allocated, SIMD-aligned FEC block —
**1 userspace copy total** (same as the theoretical minimum).

However, this requires exposing the FEC block pointer through the shim,
which is a deeper upstream coupling (see Patch 0002 in Section 4).

The simpler path (assemble a flat buffer, then call `send_packet()` which
copies into FEC) results in **2 copies** but avoids any upstream
modification.  At typical video bitrates (~500 KB/s of RTP packets) the
extra copy adds <5 µs per frame — negligible.

### Net improvement vs. piping through standalone wfb_tx

| | Standalone wfb_tx (UDP pipe) | Direct integration |
|---|---|---|
| Userspace copies | 2 (sendto kernel copy + recvmsg into wfb_tx) | 1–2 (into FEC block) |
| Syscalls per RTP pkt | 2 (sendto + recvfrom) | 0 (direct function call) |
| Process boundary | Yes (context switch) | No (same process) |
| Kernel socket buffers | 2 (TX + RX) | 0 (no UDP socket) |
| RTP stream preserved | Yes (but re-parsed by wfb_tx) | Yes (passthrough, no re-parse) |

---

## 4. Integration Strategy: Git Subtree + Minimal Patch

### Why subtree + patch (not fork, not submodule)

| Approach | Upstream sync | Build complexity | Code isolation |
|----------|--------------|------------------|----------------|
| **Fork** | Manual cherry-pick | Low | Poor — diverges over time |
| **Submodule** | `git submodule update` | Medium | Good, but patches awkward |
| **Subtree + patch** | `git subtree pull` + `git am` | Low | Best — upstream is unmodified in tree |

**Recommended approach:**

```
wfb-ng/                          ← git subtree of svpcom/wfb-ng
    src/tx.cpp                   ← upstream, unmodified
    src/tx.hpp                   ← upstream, unmodified
    src/wifibroadcast.hpp        ← upstream, unmodified
    ...
patches/
    0001-wfb-add-c-api-shim.patch  ← maintained patch series
wfb_shim/
    wfb_tx_shim.h                ← C-callable API (extern "C")
    wfb_tx_shim.cpp              ← thin wrapper, compiled as C++
```

### Sync workflow

```bash
# Pull latest upstream
git subtree pull --prefix=wfb-ng https://github.com/svpcom/wfb-ng master --squash

# Re-apply patches (fix conflicts if any)
cd wfb-ng && git am ../patches/*.patch

# Build and test
make build SOC_BUILD=star6e
```

### The patch itself — what it changes

The patch series should be minimal.  Based on analysis of tx.cpp/tx.hpp, only
these changes are needed:

**Patch 0001: Add C-linkage shim header (new file, no upstream modification)**

This adds a `wfb_tx_shim.cpp` that wraps the C++ `Transmitter` class:

```c
/* wfb_tx_shim.h — C-callable API for wfb-ng transmitter */
#ifndef WFB_TX_SHIM_H
#define WFB_TX_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wfb_tx_t wfb_tx_t;

typedef struct {
    const char *keypair_path;   /* path to tx.key */
    uint8_t     channel_id;     /* wfb channel (default 0) */
    uint8_t     fec_k;          /* data fragments per block */
    uint8_t     fec_n;          /* total fragments per block */
    uint32_t    fec_delay_us;   /* inter-FEC-packet delay */
    const char *wlan;           /* monitor-mode interface name */
} wfb_tx_config_t;

/* Create a transmitter instance. Returns NULL on failure. */
wfb_tx_t *wfb_tx_create(const wfb_tx_config_t *cfg);

/* Send a complete RTP packet (header + payload) for FEC + encryption
 * + wifi injection.  The RTP framing is preserved end-to-end so the
 * wfb-ng RX peer delivers standard RTP to the ground station decoder.
 * flags: WFB_PKT_FLAG_* from wifibroadcast.hpp (0 for normal data).
 * Returns 0 on success. */
int wfb_tx_send(wfb_tx_t *tx, const uint8_t *data, size_t len, uint8_t flags);

/* Scatter-gather variant: send RTP header + payload without flattening
 * into a contiguous buffer first.  Internally assembles into FEC block.
 * Returns 0 on success. */
int wfb_tx_send_iov(wfb_tx_t *tx,
    const uint8_t *hdr, size_t hdr_len,
    const uint8_t *payload, size_t payload_len,
    uint8_t flags);

/* Destroy transmitter and free resources. */
void wfb_tx_destroy(wfb_tx_t *tx);

#ifdef __cplusplus
}
#endif

#endif /* WFB_TX_SHIM_H */
```

The `wfb_tx_send_iov()` variant is key for zero-copy: it accepts the same
iovec-style split (12-byte RTP header on stack + payload pointing at SDK DMA
buffer) that `send_rtp_packet()` already prepares, and writes both pieces
directly into the FEC block slot — **1 copy total**, no intermediate flat
buffer needed.

**Patch 0002 (optional): Expose Transmitter FEC block pointer for direct fill**

If profiling shows even the `send_iov` path needs improvement, a second patch
could expose the raw FEC block pointer:

```cpp
// In Transmitter class (tx.hpp):
uint8_t *get_current_block_ptr(size_t *max_size);
void     commit_block_fragment(size_t actual_size, uint8_t flags);
```

This would let `send_rtp_packet()` write the RTP header + payload directly
into the FEC block buffer via two small memcpys, skipping `send_packet()`
entirely.  However, this is a deeper upstream coupling and should only be
pursued if the shim path proves insufficient.

---

## 5. HTTP API Changes in WFB Mode

### Problem: UDP-specific API fields become irrelevant

When `outgoing.server` is `wfb://wlan0`, the current `outgoing.*` API fields
(`server`, `max_payload_size`) control UDP socket behavior that no longer
applies. Meanwhile, wfb-ng has its own runtime-tunable parameters (FEC ratio,
radio modulation) exposed via a UDP command protocol in `tx_cmd.h`/`tx_cmd.c`.

### wfb-ng command API (from upstream tx_cmd.h)

wfb-ng provides a UDP-based command/response protocol for runtime control:

```c
/* Command IDs */
#define CMD_SET_FEC   1    /* Set FEC k/n ratio */
#define CMD_SET_RADIO 2    /* Set radio parameters */
#define CMD_GET_FEC   3    /* Query current FEC k/n */
#define CMD_GET_RADIO 4    /* Query current radio parameters */

/* Request structure */
typedef struct {
    uint32_t req_id;       /* random ID for request/response correlation */
    uint8_t  cmd_id;
    union {
        struct { uint8_t k; uint8_t n; }                    cmd_set_fec;
        struct {
            uint8_t stbc; bool ldpc; bool short_gi;
            uint8_t bandwidth; uint8_t mcs_index;
            bool vht_mode; uint8_t vht_nss;
        } cmd_set_radio;
    } u;
} cmd_req_t;

/* Response structure */
typedef struct {
    uint32_t req_id;       /* correlates with request */
    uint32_t rc;           /* 0 = success */
    union {
        struct { uint8_t k; uint8_t n; }                    cmd_get_fec;
        struct {
            uint8_t stbc; bool ldpc; bool short_gi;
            uint8_t bandwidth; uint8_t mcs_index;
            bool vht_mode; uint8_t vht_nss;
        } cmd_get_radio;
    } u;
} cmd_resp_t;
```

The upstream `tx_cmd.c` implements this as a standalone CLI tool that sends UDP
datagrams to `127.0.0.1:<port>`.  Since we're embedding the transmitter
directly (no separate `wfb_tx` process), we must **internalize this command
handling** into the venc HTTP API instead.

### Design: Mode-aware API field table

When WFB mode is active, the HTTP API should:
1. **Disable** UDP-specific fields that have no meaning in WFB mode.
2. **Expose** wfb-ng parameters as new live-tunable API fields.
3. **Route** set/get operations through the embedded Transmitter instead of
   a UDP command socket.

#### New config fields (WFB mode)

```c
typedef struct {
    char server[VENC_CONFIG_STRING_MAX]; /* "wfb://wlan0" */
    uint16_t max_payload_size;           /* shared: also limits wfb payload */

    /* WFB-specific (only active when server starts with "wfb://") */
    char     wfb_key[VENC_CONFIG_STRING_MAX]; /* path to tx.key */
    uint8_t  wfb_channel;                     /* wfb channel ID (default 0) */
    uint8_t  wfb_fec_k;                       /* FEC data fragments */
    uint8_t  wfb_fec_n;                       /* FEC total fragments */
    uint8_t  wfb_mcs_index;                   /* MCS rate index */
    uint8_t  wfb_stbc;                        /* space-time block coding */
    bool     wfb_ldpc;                        /* low-density parity check */
    bool     wfb_short_gi;                    /* short guard interval */
    uint8_t  wfb_bandwidth;                   /* 20/40/80 MHz */
    bool     wfb_vht_mode;                    /* VHT (802.11ac) mode */
    uint8_t  wfb_vht_nss;                     /* VHT spatial streams */
} VencConfigOutgoing;
```

#### New API field descriptors

```c
/* These fields only appear in capabilities/get/set when HAVE_WFB=1 */
#ifdef HAVE_WFB
FIELD(outgoing, wfb_key,       FT_STRING, MUT_RESTART),
FIELD(outgoing, wfb_channel,   FT_UINT,   MUT_RESTART),  /* needs session restart */
FIELD(outgoing, wfb_fec_k,    FT_UINT,   MUT_LIVE),      /* live-tunable! */
FIELD(outgoing, wfb_fec_n,    FT_UINT,   MUT_LIVE),      /* live-tunable! */
FIELD(outgoing, wfb_mcs_index, FT_UINT,  MUT_LIVE),      /* live-tunable! */
FIELD(outgoing, wfb_stbc,     FT_UINT,   MUT_LIVE),
FIELD(outgoing, wfb_ldpc,     FT_BOOL,   MUT_LIVE),
FIELD(outgoing, wfb_short_gi, FT_BOOL,   MUT_LIVE),
FIELD(outgoing, wfb_bandwidth, FT_UINT,  MUT_LIVE),
FIELD(outgoing, wfb_vht_mode, FT_BOOL,   MUT_LIVE),
FIELD(outgoing, wfb_vht_nss,  FT_UINT,   MUT_LIVE),
#endif
```

#### New apply callbacks

```c
typedef struct {
    /* ... existing callbacks ... */
    int (*apply_bitrate)(uint32_t kbps);
    int (*apply_fps)(uint32_t fps);
    int (*apply_gop)(uint32_t gop_size);
    int (*apply_roi_qp)(uint16_t qp);
    int (*apply_exposure)(uint32_t us);
    int (*apply_verbose)(bool on);
    int (*request_idr)(void);

    /* WFB-specific callbacks (NULL when not in WFB mode) */
    int (*apply_wfb_fec)(uint8_t k, uint8_t n);
    int (*apply_wfb_radio)(const VencConfigOutgoing *cfg);
} VencApplyCallbacks;
```

The backend implementation of these callbacks calls directly into the
embedded Transmitter via the C shim:

```c
/* In backend_star6e.c — WFB apply callbacks */
static int apply_wfb_fec(uint8_t k, uint8_t n) {
    return wfb_tx_set_fec(g_pipeline.wfb_tx, k, n);
}

static int apply_wfb_radio(const VencConfigOutgoing *cfg) {
    wfb_radio_cfg_t rc = {
        .stbc      = cfg->wfb_stbc,
        .ldpc      = cfg->wfb_ldpc,
        .short_gi  = cfg->wfb_short_gi,
        .bandwidth = cfg->wfb_bandwidth,
        .mcs_index = cfg->wfb_mcs_index,
        .vht_mode  = cfg->wfb_vht_mode,
        .vht_nss   = cfg->wfb_vht_nss,
    };
    return wfb_tx_set_radio(g_pipeline.wfb_tx, &rc);
}
```

#### Extended C shim API (wfb_tx_shim.h additions)

```c
/* Runtime FEC parameter change (mirrors CMD_SET_FEC). */
int wfb_tx_set_fec(wfb_tx_t *tx, uint8_t k, uint8_t n);

/* Query current FEC parameters (mirrors CMD_GET_FEC). */
int wfb_tx_get_fec(wfb_tx_t *tx, uint8_t *k, uint8_t *n);

/* Runtime radio parameter change (mirrors CMD_SET_RADIO). */
typedef struct {
    uint8_t stbc;
    bool    ldpc;
    bool    short_gi;
    uint8_t bandwidth;
    uint8_t mcs_index;
    bool    vht_mode;
    uint8_t vht_nss;
} wfb_radio_cfg_t;

int wfb_tx_set_radio(wfb_tx_t *tx, const wfb_radio_cfg_t *cfg);
int wfb_tx_get_radio(wfb_tx_t *tx, wfb_radio_cfg_t *cfg);
```

### Field visibility by mode

The `/api/v1/capabilities` endpoint should reflect which fields are active
based on the current stream mode:

| Field | UDP/RTP mode | WFB mode |
|-------|:----------:|:--------:|
| `outgoing.server` | visible (restart) | visible (restart) |
| `outgoing.max_payload_size` | visible (restart) | visible (restart) |
| `outgoing.wfb_key` | hidden | visible (restart) |
| `outgoing.wfb_channel` | hidden | visible (restart) |
| `outgoing.wfb_fec_k` | hidden | **visible (live)** |
| `outgoing.wfb_fec_n` | hidden | **visible (live)** |
| `outgoing.wfb_mcs_index` | hidden | **visible (live)** |
| `outgoing.wfb_stbc` | hidden | **visible (live)** |
| `outgoing.wfb_ldpc` | hidden | **visible (live)** |
| `outgoing.wfb_short_gi` | hidden | **visible (live)** |
| `outgoing.wfb_bandwidth` | hidden | **visible (live)** |
| `outgoing.wfb_vht_mode` | hidden | **visible (live)** |
| `outgoing.wfb_vht_nss` | hidden | **visible (live)** |

Implementation: the `handle_capabilities` handler checks the current stream
mode and skips WFB fields when not in WFB mode (and vice versa for fields
that only make sense in UDP mode).  The `handle_set`/`handle_get` handlers
return `404 not_found` for mode-inappropriate fields.

### Why not reuse upstream tx_cmd.c directly

The upstream `tx_cmd.c` is a **standalone CLI tool** that:
1. Creates a UDP socket to `127.0.0.1:<port>`.
2. Sends a `cmd_req_t` datagram.
3. Waits 3 seconds for a `cmd_resp_t` reply.
4. Prints results and exits.

This design assumes `wfb_tx` runs as a separate process listening on a UDP
command port.  In our embedded architecture, there is no separate process —
the Transmitter is instantiated directly in-process.  Therefore:

- We **do not need** the UDP command socket at all.
- We call Transmitter methods directly via the C shim (zero-overhead).
- The `cmd_req_t`/`cmd_resp_t` wire format is not needed — we pass typed
  parameters through function calls.
- The upstream `tx_cmd.c` file is **not included** in our build.

However, the **command semantics** (what parameters are tunable, their valid
ranges, their effect on the transmitter) are derived directly from
`tx_cmd.h`/`tx_cmd.c` and kept in sync with upstream changes.

---

## 6. venc-Side Code Changes

### Design: modify `send_rtp_packet()`, not the streaming loop

The key insight is that **all RTP data already flows through a single
function**: `send_rtp_packet()` (backend_star6e.c:506).  Rather than adding
a separate `send_frame_wfb()` that duplicates NAL iteration and
fragmentation logic, we modify `send_rtp_packet()` to be output-mode-aware.

This means `send_frame_rtp()` is **completely unchanged** — it continues to
do NAL extraction, start code stripping, VPS/SPS/PPS caching, and FU
fragmentation exactly as today.  Only the final packet dispatch changes.

### New stream mode

```c
/* backend_star6e.c */
enum {
    STREAM_MODE_COMPACT = 0,
    STREAM_MODE_RTP = 1,
    STREAM_MODE_WFB = 2,       /* ← new: RTP over wfb-ng */
};
```

### Modified `send_rtp_packet()` (the only functional change)

```c
static int send_rtp_packet(int socket_handle, const struct sockaddr_in* dst,
    const uint8_t* payload, size_t payload_len, RTPState* rtp, int marker
#ifdef HAVE_WFB
    , wfb_tx_t *wfb_tx    /* NULL for UDP mode, non-NULL for WFB mode */
#endif
    )
{
    if (!payload || payload_len == 0 || !rtp) return 0;

    uint8_t header[12];
    header[0] = 0x80;
    header[1] = (uint8_t)((marker ? 0x80 : 0x00) | 97);
    /* ... same RTP header build as today ... */

#ifdef HAVE_WFB
    if (wfb_tx) {
        /* WFB mode: feed RTP packet directly into FEC + wifi injection.
         * Uses iov variant to avoid flattening: header is on stack,
         * payload points at SDK DMA buffer — 1 copy into FEC block. */
        int rc = wfb_tx_send_iov(wfb_tx, header, 12,
                                  payload, payload_len, 0);
        if (rc == 0) rtp->seq++;
        return rc;
    }
#endif

    /* UDP mode: existing sendmsg path (unchanged) */
    struct iovec vec[2] = {
        {.iov_base = header, .iov_len = sizeof(header)},
        {.iov_base = (void*)payload, .iov_len = payload_len},
    };
    struct msghdr msg = {
        .msg_name = (void*)dst,
        .msg_namelen = sizeof(*dst),
        .msg_iov = vec,
        .msg_iovlen = 2,
    };
    ssize_t sent = sendmsg(socket_handle, &msg, 0);
    if (sent < 0) {
        printf("ERROR: sendmsg failed (%d)\n", errno);
        return -1;
    }
    rtp->seq++;
    return 0;
}
```

### Streaming loop change (minimal)

The streaming loop change is small — WFB mode uses `send_frame_rtp()` (same
as RTP mode) but with the wfb_tx handle propagated through:

```c
// backend_star6e.c, line ~1639
size_t total_bytes;
if (ps.stream_mode == STREAM_MODE_RTP || ps.stream_mode == STREAM_MODE_WFB)
    total_bytes = send_frame_rtp(&stream, ps.socket_handle, &ps.dst,
        &ps.rtp_state, &ps.param_sets, ps.codec
#ifdef HAVE_WFB
        , ps.wfb_tx   /* NULL in RTP mode, non-NULL in WFB mode */
#endif
        );
else
    total_bytes = send_frame_compact(&stream, ps.socket_handle,
        &ps.dst, ps.max_frame_size);
```

**No new sender function is needed.**  The `wfb_tx` pointer threads through
`send_frame_rtp()` → `send_nal_rtp_hevc()` → `send_fu_hevc()` →
`send_rtp_packet()`, where the mode branch happens at the lowest level.

### Configuration

```json
{
    "outgoing": {
        "server": "wfb://wlan0",
        "max_payload_size": 1448,
        "wfb_fec_k": 8,
        "wfb_fec_n": 12,
        "wfb_key": "/etc/drone.key"
    }
}
```

When `server` starts with `wfb://`:
- No UDP socket is created (`socket_handle` is unused).
- `wfb_tx_create()` is called during pipeline init.
- The wfb_tx handle is stored in PipelineState and threaded through the
  RTP call chain.

### PipelineState additions

```c
typedef struct {
    /* ... existing fields ... */
    wfb_tx_t *wfb_tx;          /* NULL unless stream_mode == STREAM_MODE_WFB */
} PipelineState;
```

---

## 7. Build System Integration

### New Makefile additions

```makefile
# wfb-ng integration (optional, enabled with WFB=1)
ifeq ($(WFB),1)
WFB_DIR := $(ROOT)/wfb-ng
WFB_SHIM_SRC := wfb_shim/wfb_tx_shim.cpp
WFB_CFLAGS := -I$(WFB_DIR)/src -DHAVE_WFB=1
WFB_LDFLAGS := -lstdc++ -lsodium
# zfex is part of wfb-ng source tree
WFB_OBJS := $(WFB_DIR)/src/tx.o $(WFB_DIR)/src/zfex.o wfb_shim/wfb_tx_shim.o
CFLAGS += $(WFB_CFLAGS)
endif
```

This keeps wfb-ng integration **opt-in** — existing builds are unchanged.

### Cross-compilation requirements

wfb-ng needs these for the ARM target:

| Dependency | Source | Size | Notes |
|------------|--------|------|-------|
| libsodium | Package or static build | ~300 KB | Crypto primitives |
| zfex | Bundled in wfb-ng/src/ | ~20 KB | FEC encoder (SIMD) |
| C++ runtime | Toolchain (already present) | — | glibc ARM toolchain includes libstdc++ |

**libsodium** is the only external dependency.  It can be statically linked
to avoid runtime library deployment.

---

## 8. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| **Upstream API break** in wfb-ng Transmitter class | Medium | Shim layer absorbs changes; patch rebasing is localized |
| **C++ in a C99 project** | Low | Shim compiles as C++ separately; C code only sees `extern "C"` API |
| **libsodium cross-compile** | Low | Well-supported on ARM; OpenWrt/OpenIPC both package it |
| **FEC block lifetime vs SDK buffer lifetime** | None | `send_packet()` copies into FEC block before returning; SDK buffer can be released immediately after |
| **SIMD FEC on Cortex-A7** | Low | zfex has NEON-optimized paths; falls back to scalar if unavailable |
| **Raw socket requires CAP_NET_RAW** | None | FPV systems typically run as root |
| **Monitor mode NIC setup** | Out of scope | wfb-ng provides `wfb_init` scripts; venc just needs the interface name |
| **Increased binary size** | Low | ~100 KB for wfb-ng TX + zfex + shim; libsodium adds ~300 KB if static |
| **Two codebases diverge** | Medium | Subtree + patch approach minimizes drift; patches are small and focused |

---

## 9. Performance Estimates

### Latency budget (per frame, 1080p@60 H.265 ~4 Mbit/s, avg 8 KB/frame)

| Stage | Current (RTP/UDP) | Proposed (RTP→WFB) | Delta |
|-------|-------------------|-------------------|-------|
| NAL iteration + strip | ~1 µs | ~1 µs (unchanged) | — |
| RTP header build | ~0.5 µs | ~0.5 µs (unchanged) | — |
| FU fragmentation | ~1 µs | ~1 µs (unchanged) | — |
| memcpy to FEC block | 0 | ~2 µs (8 KB via send_iov) | +2 µs |
| FEC encode (k=8, n=12) | 0 | ~10 µs (NEON) | +10 µs |
| Encryption (per fragment) | 0 | ~1 µs × 12 = 12 µs | +12 µs |
| UDP sendmsg() | ~5 µs × ~7 pkts | 0 (no UDP socket) | -35 µs |
| Raw socket sendmsg() | 0 | ~3 µs × 12 = 36 µs | +36 µs |
| **Total per frame** | **~37.5 µs** | **~62.5 µs** | **+25 µs** |

The added latency is negligible (0.025 ms) compared to the frame interval
(16.7 ms at 60 FPS).  The benefit is FEC redundancy and encrypted wireless
transport with no intermediate process, no extra UDP hop, and standard RTP
preserved end-to-end for the ground station decoder.

### Throughput

At 4 Mbit/s with FEC 8/12, the effective air rate is 6 Mbit/s — well within
the capacity of a single 20 MHz 802.11n channel.  No bottleneck expected.

---

## 10. Implementation Plan

### Phase 1: Subtree setup and shim (no venc changes)

1. Add wfb-ng as a git subtree under `wfb-ng/`.
2. Create `wfb_shim/wfb_tx_shim.{h,cpp}` with the C-callable API.
3. Create `patches/0001-wfb-add-c-api-shim.patch`.
4. Verify cross-compilation of the shim + wfb-ng TX code for ARM.
5. Verify libsodium static linking.

### Phase 2: venc integration (behind WFB=1 flag)

1. Add `STREAM_MODE_WFB` enum value.
2. Extend `VencConfigOutgoing` with wfb-specific fields.
3. Add `wfb_tx` parameter to `send_rtp_packet()` → `send_nal_rtp_hevc()`
   → `send_fu_hevc()` → `send_frame_rtp()` call chain (compile-guarded).
4. Add WFB mode branch in `send_rtp_packet()`: call `wfb_tx_send_iov()`
   instead of `sendmsg()`.
5. Skip UDP socket creation when `server` starts with `wfb://`.
6. Update config parser for `wfb://` URI scheme.
7. Update Makefile with opt-in `WFB=1` build flag.

### Phase 3: HTTP API integration

1. Add wfb field descriptors to `g_fields[]` (compile-guarded by `HAVE_WFB`).
2. Add `apply_wfb_fec` and `apply_wfb_radio` callbacks to `VencApplyCallbacks`.
3. Implement mode-aware field visibility in `handle_capabilities`.
4. Implement backend apply callbacks that call into the C shim.
5. Extend C shim with `wfb_tx_set_fec()`, `wfb_tx_get_fec()`,
   `wfb_tx_set_radio()`, `wfb_tx_get_radio()`.
6. Return `404 not_found` for WFB fields when in UDP/RTP mode (and vice versa).

### Phase 4: Testing and validation

1. Unit tests for config parsing of `wfb://` URIs.
2. Unit tests for mode-aware field visibility.
3. Cross-compile with `WFB=1` and verify binary runs on target.
4. End-to-end test: venc → wfb-ng TX → wfb-ng RX → video decode.
5. Live FEC/radio parameter change via HTTP API during streaming.
6. Latency measurement (compare RTP/UDP vs WFB direct).

### Phase 5: Upstream sync validation

1. Pull latest wfb-ng master via `git subtree pull`.
2. Rebase patches.
3. Verify build still works.
4. Document the sync procedure.

---

## 11. Alternatives Considered

### A. Pipe to wfb_tx process (status quo approach)

```
venc → UDP → wfb_tx (separate process) → radio
```

**Pros:** No code integration, upstream-independent.
**Cons:** Extra process, extra UDP hop (kernel copy), no buffer sharing,
harder to coordinate FEC parameters with encoder frame boundaries.

### B. Shared memory ring buffer

```
venc → shm ring → wfb_tx reads from ring → radio
```

**Pros:** True zero-copy.
**Cons:** Complex synchronization, still needs separate process or thread,
FEC still needs its own aligned buffers (so the "zero-copy" benefit is
limited to avoiding one memcpy that takes ~2 µs).

### C. Kernel-space FEC (mac80211 modification)

**Pros:** Truly zero-copy end-to-end.
**Cons:** Requires custom kernel module, unmaintainable, not portable.

### Recommendation

Option in this study (direct library call via C shim) provides the best
balance of performance, maintainability, and upstream compatibility.

---

## 12. Open Questions

1. **libsodium packaging:** Is libsodium already available in the OpenIPC
   rootfs, or does it need to be statically linked?

2. **Monitor mode setup:** Should venc manage the wireless interface
   (set monitor mode, channel, TX power), or assume it is pre-configured
   by system scripts?

3. **Dual output:** Should wfb mode be exclusive, or should venc support
   simultaneous RTP + WFB output (useful for local recording + wireless TX)?

4. **Session key distribution:** wfb-ng TX announces session keys over the
   air.  The corresponding RX needs the keypair.  Is key management in scope
   for this integration, or handled externally?
