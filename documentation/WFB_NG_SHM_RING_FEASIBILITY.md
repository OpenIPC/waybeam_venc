commit 62f401a4edf0843528d95bba13b1d8d2354d3b02
Author: Claude <noreply@anthropic.com>
Date:   Tue Mar 10 13:46:06 2026 +0000

    Add feasibility study for SHM SPSC ring buffer wfb-ng integration
    
    Alternative to direct library embedding: venc writes RTP packets into a
    POSIX shared memory ring buffer, a modified wfb_tx reads from it.
    Zero syscalls in steady-state (futex only on stream gaps), full process
    isolation, pure C99 in venc, and tx_cmd integrated as direct controller.
    
    https://claude.ai/code/session_01AYoQ54XF2WwDRycfv18wYB

diff --git a/documentation/WFB_NG_SHM_RING_FEASIBILITY.md b/documentation/WFB_NG_SHM_RING_FEASIBILITY.md
new file mode 100644
index 0000000..6db76e3
--- /dev/null
+++ b/documentation/WFB_NG_SHM_RING_FEASIBILITY.md
@@ -0,0 +1,784 @@
+# WFB-NG Integration via Shared Memory SPSC Ring Buffer — Feasibility Study
+
+## 1. Executive Summary
+
+This study evaluates an **alternative integration path** for wfb-ng: instead
+of embedding wfb-ng's C++ transmitter directly into the venc process (see
+`WFB_NG_INTEGRATION_FEASIBILITY.md`), this approach uses a **POSIX shared
+memory SPSC (Single-Producer Single-Consumer) ring buffer** between venc
+(producer) and a modified `wfb_tx` process (consumer).
+
+**Verdict: feasible and architecturally cleaner**, with stronger process
+isolation, zero C++ in the venc build, and near-zero syscall overhead during
+steady-state streaming.  The cost is one additional `memcpy` (~2 µs per frame)
+compared to the direct library call approach, and the requirement for a
+**lightly modified `wfb_tx` binary** that reads from shared memory instead of
+(or in addition to) UDP.
+
+The `tx_cmd` control interface can be integrated directly into venc's HTTP API
+(or waybeam) as a thin UDP command sender — no process embedding required.
+
+---
+
+## 2. Architecture Overview
+
+### Design
+
+```
+┌─────────────────────────────────────┐   ┌──────────────────────────────────┐
+│ venc (producer)                     │   │ wfb_tx (consumer)                │
+│                                     │   │                                  │
+│ MI_VENC_GetStream()                 │   │ poll shm ring (spin/futex)       │
+│   │                                 │   │   │                              │
+│   ▼                                 │   │   ▼                              │
+│ send_frame_rtp()                    │   │ read RTP packet from slot        │
+│   │  (NAL iterate, FU frag,         │   │   │                              │
+│   │   VPS/SPS/PPS — all unchanged)  │   │   ▼                              │
+│   │                                 │   │ Transmitter::send_packet()       │
+│   ▼                                 │   │   │  FEC encode                  │
+│ send_rtp_packet()                   │   │   │  ChaCha20 encrypt            │
+│   │  build 12-byte RTP header       │   │   │  802.11 frame build          │
+│   │  memcpy hdr+payload → ring slot │   │   ▼                              │
+│   │  advance write index            │   │ inject_packet()                  │
+│   │  (futex wake ONLY if consumer   │   │   └─ raw socket → NIC           │
+│   │   was sleeping)                 │   │                                  │
+│   ▼                                 │   │                                  │
+│ MI_VENC_ReleaseStream()             │   │                                  │
+│                                     │   │                                  │
+│ ┌─────────────────────────────────┐ │   │                                  │
+│ │ tx_cmd sender (integrated)      │ │   │ ┌──────────────────────────────┐ │
+│ │ HTTP API or waybeam calls       │─┼───┼─┤ UDP cmd port (127.0.0.1)    │ │
+│ │ send_command() from tx_cmd.c    │ │   │ │ CMD_SET_FEC, CMD_SET_RADIO  │ │
+│ └─────────────────────────────────┘ │   │ └──────────────────────────────┘ │
+└─────────────────────────────────────┘   └──────────────────────────────────┘
+            │                                         │
+            └─── /dev/shm/venc_wfb_ring ──────────────┘
+                  (POSIX shared memory, mmap'd)
+```
+
+### Key properties
+
+- **Process isolation**: venc and wfb_tx are separate processes.  A crash in
+  wfb_tx (raw socket error, FEC assertion, crypto issue) does not bring down
+  the encoder.  venc can detect the consumer is gone and continue running
+  (dropping packets or falling back to UDP).
+- **Zero C++ in venc**: The ring buffer is pure C99.  All wfb-ng C++ code
+  stays in the wfb_tx process.
+- **Upstream wfb_tx preserved**: The modification to wfb_tx is a small
+  additional input mode (read from shm instead of UDP socket) alongside the
+  existing UDP path.  No changes to FEC, crypto, or injection code.
+- **Standard RTP preserved**: The ring carries complete RTP packets (header +
+  payload).  wfb_tx treats them as opaque data — identical to UDP-received
+  packets.
+
+---
+
+## 3. SPSC Ring Buffer Design
+
+### Memory layout
+
+```
+┌────────────────────────────────────────────────────────────────┐
+│ Ring Header (cache-line aligned, 64 bytes)                     │
+│ ┌──────────────────────────────────────────────────────────┐   │
+│ │ uint32_t magic          (0x56454E43 = "VENC")            │   │
+│ │ uint32_t version        (1)                              │   │
+│ │ uint32_t slot_count     (power of 2, e.g. 256)           │   │
+│ │ uint32_t slot_size      (e.g. 1500 bytes)                │   │
+│ │ uint32_t ring_size      (total mmap size)                │   │
+│ │ uint32_t _pad[3]        (align to 32 bytes)              │   │
+│ │                                                          │   │
+│ │ /* Separate cache lines to avoid false sharing */        │   │
+│ │ uint64_t write_idx      (producer-owned, cache line 1)   │   │
+│ │ uint8_t  _pad_w[56]                                      │   │
+│ │ uint64_t read_idx       (consumer-owned, cache line 2)   │   │
+│ │ uint8_t  _pad_r[56]                                      │   │
+│ │                                                          │   │
+│ │ /* Consumer sleep flag (for futex) */                     │   │
+│ │ uint32_t consumer_waiting  (0 = spinning, 1 = sleeping)  │   │
+│ │ uint8_t  _pad_f[60]     (cache line 3)                   │   │
+│ └──────────────────────────────────────────────────────────┘   │
+│                                                                │
+│ Slot Array (slot_count × slot_size)                            │
+│ ┌──────────────────────────────────────┐                       │
+│ │ Slot 0:                              │                       │
+│ │   uint16_t length  (actual payload)  │                       │
+│ │   uint8_t  data[slot_size - 2]       │                       │
+│ ├──────────────────────────────────────┤                       │
+│ │ Slot 1: ...                          │                       │
+│ ├──────────────────────────────────────┤                       │
+│ │ ...                                  │                       │
+│ └──────────────────────────────────────┘                       │
+└────────────────────────────────────────────────────────────────┘
+```
+
+### Sizing
+
+For 1080p@60 H.265 at ~4 Mbit/s:
+- Average frame size: ~8 KB
+- RTP packets per frame: ~7 (at 1200-byte MTU)
+- Packets per second: ~420
+- At 256 slots × 1500 bytes = 384 KB of ring data + header
+- Ring holds ~600 ms of video at full rate — ample headroom
+
+Total shared memory: **~400 KB** (header + slots).
+
+### Synchronization protocol
+
+The SPSC ring uses **acquire/release memory ordering** on the index variables
+and a **futex** for sleeping when the ring is empty.
+
+**Producer (venc) — write path:**
+
+```c
+static inline int venc_ring_write(venc_ring_t *ring,
+                                  const void *data, uint16_t len)
+{
+    uint64_t w = __atomic_load_n(&ring->hdr->write_idx, __ATOMIC_RELAXED);
+    uint64_t r = __atomic_load_n(&ring->hdr->read_idx, __ATOMIC_ACQUIRE);
+
+    if (w - r >= ring->hdr->slot_count) {
+        return -1;  /* ring full — drop packet (consumer too slow) */
+    }
+
+    uint32_t idx = (uint32_t)(w & (ring->hdr->slot_count - 1));
+    ring_slot_t *slot = &ring->slots[idx];
+    slot->length = len;
+    memcpy(slot->data, data, len);
+
+    __atomic_store_n(&ring->hdr->write_idx, w + 1, __ATOMIC_RELEASE);
+
+    /* Wake consumer ONLY if it was sleeping */
+    if (__atomic_load_n(&ring->hdr->consumer_waiting, __ATOMIC_ACQUIRE)) {
+        syscall(SYS_futex, &ring->hdr->write_idx,
+                FUTEX_WAKE, 1, NULL, NULL, 0);
+    }
+
+    return 0;
+}
+```
+
+**Consumer (wfb_tx) — read path:**
+
+```c
+static inline int wfb_ring_read(venc_ring_t *ring,
+                                void *buf, uint16_t *len, int timeout_ms)
+{
+    uint64_t r = __atomic_load_n(&ring->hdr->read_idx, __ATOMIC_RELAXED);
+
+    for (;;) {
+        uint64_t w = __atomic_load_n(&ring->hdr->write_idx, __ATOMIC_ACQUIRE);
+
+        if (r < w) {
+            /* Data available — read it */
+            uint32_t idx = (uint32_t)(r & (ring->hdr->slot_count - 1));
+            ring_slot_t *slot = &ring->slots[idx];
+            *len = slot->length;
+            memcpy(buf, slot->data, slot->length);
+
+            __atomic_store_n(&ring->hdr->read_idx, r + 1, __ATOMIC_RELEASE);
+            return 0;
+        }
+
+        /* Ring empty — sleep on futex (sets the waiting flag first) */
+        __atomic_store_n(&ring->hdr->consumer_waiting, 1, __ATOMIC_RELEASE);
+
+        struct timespec ts = { .tv_sec = timeout_ms / 1000,
+                               .tv_nsec = (timeout_ms % 1000) * 1000000L };
+        syscall(SYS_futex, &ring->hdr->write_idx,
+                FUTEX_WAIT, (uint32_t)w, &ts, NULL, 0);
+
+        __atomic_store_n(&ring->hdr->consumer_waiting, 0, __ATOMIC_RELEASE);
+    }
+}
+```
+
+---
+
+## 4. Syscall Analysis — When Does the (1) Futex Wake Happen?
+
+### Steady-state continuous streaming: **zero syscalls**
+
+During normal 60fps streaming, the producer writes ~7 RTP packets per frame
+(one every ~2.4 ms).  The consumer processes each packet in ~10–50 µs (FEC +
+encrypt + inject).  Since the consumer is always faster than the producer, it
+immediately finds data on each iteration:
+
+```
+Time ──────────────────────────────────────────────────────────────►
+
+Producer:  W  W  W  W  W  W  W  │  W  W  W  W  W  W  W  │
+           ↑  ↑  ↑  ↑  ↑  ↑  ↑  │  ↑  ↑  ↑  ↑  ↑  ↑  ↑  │
+Consumer:   R  R  R  R  R  R  R  │   R  R  R  R  R  R  R  │
+                                 │                          │
+           ├──── frame N ────────┤──── frame N+1 ───────────┤
+```
+
+The consumer **never sleeps** because `w > r` is always true when it checks.
+Therefore:
+- `consumer_waiting` is always 0
+- The producer never calls `FUTEX_WAKE`
+- The consumer never calls `FUTEX_WAIT`
+- **Total syscalls for the ring: 0**
+
+The only syscalls per packet are the consumer's `sendmsg()` to the raw socket
+for wifi injection — which is unavoidable regardless of integration approach.
+
+### When the futex wake actually fires
+
+The (1) syscall happens only in these transient scenarios:
+
+| Scenario | Frequency | Duration |
+|----------|-----------|----------|
+| **Stream startup** | Once | Consumer starts before producer has frames; sleeps until first frame arrives |
+| **Encoder stall** | Rare | SDK returns no packs (`MI_VENC_Query` returns curPacks=0); consumer drains ring and sleeps until next frame |
+| **Output re-enable** | On toggle | `output_enabled` goes from false→true; first frame wakes consumer |
+| **Pipeline reinit** | On config change | 200ms debounce gap; consumer sleeps, wakes when new pipeline starts |
+| **FPS drop** | On FPS change | If FPS drops (e.g. 60→30), inter-frame gap doubles; consumer *might* sleep briefly between frames if it finishes the current batch before the next arrives |
+
+**In practice at 60fps**: the inter-frame gap is 16.7ms and the consumer
+processes all packets in <1ms.  Even if the consumer finishes early, it can
+**spin-wait** for a configurable number of iterations (e.g. 1000 × `pause`
+instructions, ~10 µs) before falling back to futex sleep.  With this hybrid
+approach, the futex syscall essentially never fires during streaming.
+
+### Comparison with UDP socket approach
+
+| | UDP pipe (sendto/recvfrom) | Shared memory ring |
+|---|---|---|
+| Syscalls per RTP packet | **2** (sendto + recvfrom) | **0** (steady state) |
+| Syscalls per frame (7 pkts) | **14** | **0** |
+| Syscalls per second (60fps) | **~840** | **0** (+ rare futex on gaps) |
+| Kernel buffer copies | 2 per packet | 0 |
+| Context switches | Every recvfrom blocks | 0 (spin) or rare futex |
+
+This is the primary advantage of the shared memory approach over piping
+through a UDP socket: **eliminating ~840 syscalls/second** and their
+associated kernel overhead (socket buffer management, skb alloc/free,
+scheduling).
+
+---
+
+## 5. Zero-Copy Analysis
+
+### Copy count per RTP packet
+
+| Step | Copy? | Notes |
+|------|-------|-------|
+| SDK DMA buf → RTP header build | **No** | iovec assembly, same as today |
+| RTP header (12B) + payload → ring slot | **Yes** | `memcpy` into ring slot (~1.2 KB) |
+| Ring slot → FEC block (consumer) | **Yes** | `send_packet()` copies into SIMD-aligned FEC buffer |
+| FEC encode → encrypt | **No** | In-place |
+| Encrypted buf → raw socket | **No** | Scatter-gather iovec |
+
+**Total: 2 userspace copies** per RTP packet.
+
+### Compared to other approaches
+
+| Approach | Userspace copies | Syscalls/pkt | Process boundary |
+|----------|:---------------:|:------------:|:----------------:|
+| **UDP pipe** (current wfb_tx) | 2 | 2 (sendto+recvfrom) | Yes |
+| **Direct library call** (WFB_NG_INTEGRATION_FEASIBILITY) | 1 | 0 | No |
+| **SHM ring** (this study) | 2 | 0 | Yes |
+
+The SHM ring has the same copy count as UDP pipe but **eliminates all
+syscalls**.  The direct library call saves one copy (writes directly into FEC
+block) but sacrifices process isolation.
+
+### Can the SHM ring achieve 1 copy?
+
+Yes, with a **direct FEC block fill** protocol: the consumer exposes its FEC
+block pointers through shared memory, and the producer writes directly into
+them.  However, this tightly couples venc to wfb-ng's internal FEC buffer
+layout and SIMD alignment requirements — fragile and not recommended.
+
+The extra copy (~2 µs for 1.2 KB on Cortex-A7) is negligible relative to the
+16.7 ms frame interval.
+
+---
+
+## 6. Modified `wfb_tx` Binary
+
+### What changes
+
+The existing `wfb_tx` process reads data from a UDP socket via `poll()` +
+`recvmsg()`.  The modification adds an **alternative input mode** that reads
+from the shared memory ring buffer instead.
+
+The change is localized to the `data_source()` function in `tx.cpp`:
+
+```cpp
+// In data_source(), replace or augment the UDP recvmsg path:
+
+#ifdef HAVE_SHM_INPUT
+if (shm_ring) {
+    // Replace poll() on UDP fd with ring buffer read
+    uint16_t pkt_len;
+    int rc = wfb_ring_read(shm_ring, pkt_buf, &pkt_len, poll_timeout);
+    if (rc == 0) {
+        send_packet(pkt_buf, pkt_len, 0);
+    }
+    // Control channel still uses its own UDP socket (unchanged)
+    if (control_fd >= 0) {
+        // ... existing CMD_SET_FEC / CMD_SET_RADIO handling ...
+    }
+} else
+#endif
+{
+    // Existing UDP recvmsg path (unchanged)
+    int rc = poll(fds, nfds + 1, poll_timeout);
+    // ...
+}
+```
+
+### What does NOT change in wfb_tx
+
+- FEC encoding (zfex) — unchanged
+- Encryption (libsodium ChaCha20-Poly1305) — unchanged
+- Raw socket injection — unchanged
+- Session key management — unchanged
+- Command channel (tx_cmd) — unchanged, still UDP on localhost
+- Radiotap header construction — unchanged
+- Statistics logging — unchanged
+
+### Patch size estimate
+
+~80 lines of new code in `tx.cpp`:
+- Ring buffer reader (the C header `venc_ring.h` is shared with venc)
+- CLI flag `--shm-input /dev/shm/venc_wfb_ring`
+- Fallback to UDP if ring open fails
+
+Plus the shared `venc_ring.h` header (~100 lines) that both processes include.
+
+### Deployment
+
+The modified `wfb_tx` binary is deployed alongside venc on the target system.
+It runs as a **separate process** (systemd unit, init script, or manually):
+
+```bash
+# Start wfb_tx with shared memory input
+wfb_tx --shm-input /dev/shm/venc_wfb_ring \
+       -k 8 -n 12 -K /etc/drone.key \
+       -u 9000   # command port for tx_cmd control
+       wlan0
+
+# Start venc with SHM output
+venc --config /etc/venc.json
+# (venc.json has: "outgoing": { "server": "shm:///dev/shm/venc_wfb_ring" })
+```
+
+Process startup order does not matter:
+- If wfb_tx starts first, it blocks on `FUTEX_WAIT` until venc writes.
+- If venc starts first, it writes to the ring; packets accumulate (up to
+  slot_count) until wfb_tx starts reading.  If the ring fills before wfb_tx
+  starts, packets are dropped (harmless for video — the decoder handles gaps).
+
+---
+
+## 7. tx_cmd Integration as Direct WFB Controller
+
+### The opportunity
+
+The upstream `tx_cmd.c` is a tiny CLI tool (~300 lines) that sends UDP
+command datagrams to a running `wfb_tx` process on `127.0.0.1:<port>`.  Its
+functionality maps directly to operations needed by the venc HTTP API and
+waybeam:
+
+| tx_cmd operation | HTTP API equivalent | waybeam equivalent |
+|-----------------|--------------------|--------------------|
+| `set_fec -k 8 -n 12` | `GET /api/v1/set?wfb_fec_k=8&wfb_fec_n=12` | MAVLink command |
+| `set_radio -M 3 -B 40` | `GET /api/v1/set?wfb_mcs_index=3&wfb_bandwidth=40` | MAVLink command |
+| `get_fec` | `GET /api/v1/get?wfb_fec_k` | MAVLink telemetry |
+| `get_radio` | `GET /api/v1/get?wfb_mcs_index` | MAVLink telemetry |
+
+### Integration approach: embed `send_command()` into venc
+
+Rather than shelling out to the `tx_cmd` binary, extract the core
+`send_command()` function (~40 lines of C) and integrate it directly:
+
+```c
+/* wfb_cmd.h — lightweight wfb_tx command sender (pure C99) */
+#ifndef WFB_CMD_H
+#define WFB_CMD_H
+
+#include <stdint.h>
+#include <stdbool.h>
+
+/* Wire-compatible with wfb-ng tx_cmd.h */
+typedef struct __attribute__((packed)) {
+    uint32_t req_id;
+    uint8_t  cmd_id;
+    union {
+        struct { uint8_t k; uint8_t n; }  set_fec;
+        struct {
+            uint8_t stbc; uint8_t ldpc; uint8_t short_gi;
+            uint8_t bandwidth; uint8_t mcs_index;
+            uint8_t vht_mode; uint8_t vht_nss;
+        } set_radio;
+    } u;
+} wfb_cmd_req_t;
+
+typedef struct __attribute__((packed)) {
+    uint32_t req_id;
+    uint32_t rc;       /* 0 = success, errno otherwise */
+    union {
+        struct { uint8_t k; uint8_t n; }  get_fec;
+        struct {
+            uint8_t stbc; uint8_t ldpc; uint8_t short_gi;
+            uint8_t bandwidth; uint8_t mcs_index;
+            uint8_t vht_mode; uint8_t vht_nss;
+        } get_radio;
+    } u;
+} wfb_cmd_resp_t;
+
+/* Send a command to wfb_tx and wait for response.
+ * port: wfb_tx command UDP port (e.g. 9000).
+ * timeout_ms: max wait (e.g. 500).  Returns 0 on success. */
+int wfb_cmd_send(int port, wfb_cmd_req_t *req, size_t req_size,
+                 wfb_cmd_resp_t *resp, int timeout_ms);
+
+/* Convenience wrappers */
+int wfb_cmd_set_fec(int port, uint8_t k, uint8_t n);
+int wfb_cmd_get_fec(int port, uint8_t *k, uint8_t *n);
+int wfb_cmd_set_radio(int port, uint8_t stbc, uint8_t ldpc,
+                      uint8_t short_gi, uint8_t bandwidth,
+                      uint8_t mcs_index, uint8_t vht_mode,
+                      uint8_t vht_nss);
+int wfb_cmd_get_radio(int port, uint8_t *stbc, uint8_t *ldpc,
+                      uint8_t *short_gi, uint8_t *bandwidth,
+                      uint8_t *mcs_index, uint8_t *vht_mode,
+                      uint8_t *vht_nss);
+
+#endif /* WFB_CMD_H */
+```
+
+### Implementation: ~60 lines
+
+The `send_command()` function from upstream `tx_cmd.c` translates almost
+directly.  Key simplifications for embedding:
+
+1. **No alarm/signal handler** — use `setsockopt(SO_RCVTIMEO)` instead.
+2. **No getopt** — parameters come from typed function arguments.
+3. **Reuse socket** — keep a persistent UDP socket rather than creating one
+   per command (the upstream CLI creates+destroys per invocation).
+
+```c
+/* wfb_cmd.c */
+#include "wfb_cmd.h"
+#include <sys/socket.h>
+#include <netinet/in.h>
+#include <string.h>
+#include <unistd.h>
+#include <stdlib.h>
+#include <errno.h>
+#include <stddef.h>
+
+int wfb_cmd_send(int port, wfb_cmd_req_t *req, size_t req_size,
+                 wfb_cmd_resp_t *resp, int timeout_ms)
+{
+    int fd = socket(AF_INET, SOCK_DGRAM, 0);
+    if (fd < 0) return -errno;
+
+    struct timeval tv = {
+        .tv_sec  = timeout_ms / 1000,
+        .tv_usec = (timeout_ms % 1000) * 1000
+    };
+    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
+
+    struct sockaddr_in addr = {
+        .sin_family = AF_INET,
+        .sin_port   = htons((uint16_t)port),
+        .sin_addr   = { .s_addr = htonl(0x7f000001) }  /* 127.0.0.1 */
+    };
+
+    ssize_t sent = sendto(fd, req, req_size, 0,
+                          (struct sockaddr *)&addr, sizeof(addr));
+    if (sent < 0) { close(fd); return -errno; }
+
+    memset(resp, 0, sizeof(*resp));
+    ssize_t rcvd = recv(fd, resp, sizeof(*resp), 0);
+    close(fd);
+
+    if (rcvd < (ssize_t)offsetof(wfb_cmd_resp_t, u))
+        return -EPROTO;
+    if (resp->req_id != req->req_id)
+        return -EPROTO;
+    if (resp->rc != 0)
+        return -(int)ntohl(resp->rc);
+
+    return 0;
+}
+
+int wfb_cmd_set_fec(int port, uint8_t k, uint8_t n) {
+    wfb_cmd_req_t req = { .req_id = htonl((uint32_t)rand()), .cmd_id = 1 };
+    req.u.set_fec.k = k;
+    req.u.set_fec.n = n;
+    wfb_cmd_resp_t resp;
+    return wfb_cmd_send(port, &req,
+        offsetof(wfb_cmd_req_t, u) + sizeof(req.u.set_fec), &resp, 500);
+}
+/* ... similar for get_fec, set_radio, get_radio ... */
+```
+
+### HTTP API integration via apply callbacks
+
+```c
+/* In backend_star6e.c */
+static int g_wfb_cmd_port = 9000;  /* from config */
+
+static int apply_wfb_fec(uint8_t k, uint8_t n) {
+    return wfb_cmd_set_fec(g_wfb_cmd_port, k, n);
+}
+
+static int apply_wfb_radio(const VencConfigOutgoing *cfg) {
+    return wfb_cmd_set_radio(g_wfb_cmd_port,
+        cfg->wfb_stbc, cfg->wfb_ldpc, cfg->wfb_short_gi,
+        cfg->wfb_bandwidth, cfg->wfb_mcs_index,
+        cfg->wfb_vht_mode, cfg->wfb_vht_nss);
+}
+```
+
+The HTTP API routes `/api/v1/set?wfb_fec_k=8` etc. trigger these callbacks,
+which send the UDP command to the running `wfb_tx` process.  This is the
+**same protocol** that upstream `tx_cmd` uses — fully wire-compatible.
+
+### waybeam integration
+
+waybeam (or any MAVLink ground station) can use the same `wfb_cmd_send()`
+function to control wfb_tx directly, or route through the venc HTTP API.
+The choice depends on whether waybeam runs on the same device as wfb_tx:
+
+- **Same device**: Call `wfb_cmd_set_fec()` directly (localhost UDP).
+- **Remote**: Route through venc HTTP API → apply callback → UDP cmd.
+
+---
+
+## 8. venc-Side Code Changes
+
+### New stream mode
+
+```c
+enum {
+    STREAM_MODE_COMPACT = 0,
+    STREAM_MODE_RTP     = 1,
+    STREAM_MODE_SHM     = 2,   /* ← new: RTP over shared memory ring */
+};
+```
+
+### Modified `send_rtp_packet()` — SHM path
+
+```c
+static int send_rtp_packet(int socket_handle, const struct sockaddr_in *dst,
+    const uint8_t *payload, size_t payload_len, RTPState *rtp, int marker,
+    venc_ring_t *ring   /* NULL for UDP mode, non-NULL for SHM mode */
+    )
+{
+    if (!payload || payload_len == 0 || !rtp) return 0;
+
+    uint8_t header[12];
+    /* ... same RTP header build as today ... */
+
+    if (ring) {
+        /* SHM mode: assemble RTP packet into ring slot */
+        uint8_t pkt[12 + 1200];   /* max RTP packet on stack */
+        memcpy(pkt, header, 12);
+        memcpy(pkt + 12, payload, payload_len);
+        int rc = venc_ring_write(ring, pkt, (uint16_t)(12 + payload_len));
+        if (rc == 0) rtp->seq++;
+        return rc;
+    }
+
+    /* UDP mode: existing sendmsg path (unchanged) */
+    struct iovec vec[2] = { ... };
+    struct msghdr msg = { ... };
+    ssize_t sent = sendmsg(socket_handle, &msg, 0);
+    ...
+}
+```
+
+### Pipeline init changes
+
+When `server` starts with `shm://`:
+
+```c
+/* Skip UDP socket creation */
+if (strncmp(vcfg.outgoing.server, "shm://", 6) == 0) {
+    const char *shm_path = vcfg.outgoing.server + 6;
+    ps.ring = venc_ring_create(shm_path, 256, 1500);
+    if (!ps.ring) {
+        printf("ERROR: failed to create SHM ring at %s\n", shm_path);
+        goto cleanup_sys;
+    }
+    ps.stream_mode = STREAM_MODE_SHM;
+    /* No socket_handle needed */
+} else {
+    /* Existing UDP socket creation */
+    ...
+}
+```
+
+### PipelineState additions
+
+```c
+typedef struct {
+    /* ... existing fields ... */
+    venc_ring_t *ring;      /* NULL unless stream_mode == STREAM_MODE_SHM */
+    int wfb_cmd_port;       /* wfb_tx command UDP port (for tx_cmd control) */
+} PipelineState;
+```
+
+### New files in venc
+
+| File | Lines | Purpose |
+|------|-------|---------|
+| `venc_ring.h` | ~100 | Ring buffer header (shared with wfb_tx) |
+| `venc_ring.c` | ~80 | Ring create/destroy, POSIX shm setup |
+| `wfb_cmd.h` | ~50 | tx_cmd wire protocol + convenience API |
+| `wfb_cmd.c` | ~80 | UDP command sender |
+
+Total: **~310 lines of pure C99**, no external dependencies.
+
+---
+
+## 9. Comparison: SHM Ring vs Direct Library Call
+
+| Dimension | Direct Library Call | SHM Ring |
+|-----------|:------------------:|:--------:|
+| **Copies per RTP packet** | 1 | 2 |
+| **Syscalls per packet** | 0 | 0 (steady state) |
+| **Process isolation** | None (same process) | Full (separate processes) |
+| **Crash containment** | wfb crash kills encoder | wfb crash independent |
+| **C++ in venc** | Yes (shim compiles as C++) | No (pure C99) |
+| **Build dependencies** | libsodium, libstdc++, zfex | None new |
+| **Binary size delta** | ~400 KB (wfb+sodium static) | ~10 KB (ring+cmd) |
+| **Upstream wfb_tx coupling** | High (embedded Transmitter) | Low (shm read + existing cmd) |
+| **wfb_tx modification** | Not used (replaced) | Small (~80 lines added) |
+| **API parameter control** | Direct function call (0 latency) | UDP loopback (~50 µs round-trip) |
+| **Latency per frame** | ~62.5 µs | ~64.5 µs (+2 µs for extra copy) |
+| **Recovery from wfb failure** | Must restart entire venc | Restart wfb_tx only; venc detects and drops |
+| **Dual output (RTP+WFB)** | Complex (two Transmitter instances) | Trivial (keep UDP socket open alongside ring) |
+
+### When to prefer SHM ring
+
+- Process isolation is valued (production FPV systems where encoder uptime
+  is critical)
+- You want to keep venc as a pure C99 project
+- You want minimal coupling to wfb-ng internals
+- You want independent upgrade/restart of wfb_tx without touching the encoder
+- You want dual output (e.g., SHM to wfb_tx + UDP to local recorder)
+
+### When to prefer direct library call
+
+- Absolute minimum latency is required (saves ~2 µs per frame)
+- Single binary deployment is preferred
+- You want direct API control with zero round-trip latency
+
+---
+
+## 10. Risks and Mitigations
+
+| Risk | Severity | Mitigation |
+|------|----------|------------|
+| **Consumer process dies** | Medium | Producer detects full ring (write returns -1), logs warning, drops frames. Auto-restart via systemd `Restart=always`. |
+| **Producer process dies** | Low | Consumer blocks on empty ring (futex timeout), logs warning, waits for producer restart. |
+| **Shared memory corruption** | Low | Magic + version header validated on open. Consumer ignores corrupted slots (length > slot_size). |
+| **Cache coherency (ARM)** | None | `__atomic_*` builtins emit proper DMB instructions on ARMv7. |
+| **False sharing** | None | Write index and read index on separate 64-byte cache lines. |
+| **Stale shm file** | Low | `shm_unlink()` on clean shutdown; `O_CREAT\|O_TRUNC` on producer startup. |
+| **ABI drift** | Low | Ring header contains version field; consumer validates before attaching. |
+| **tx_cmd UDP timeout** | Low | 500ms timeout with error propagation to HTTP API caller; non-blocking to main loop. |
+
+---
+
+## 11. Performance Estimates
+
+### Latency budget (per frame, 1080p@60 H.265 ~4 Mbit/s, ~8 KB/frame)
+
+| Stage | Current (RTP/UDP) | SHM Ring | Delta |
+|-------|:-----------------:|:--------:|:-----:|
+| NAL iteration + strip | ~1 µs | ~1 µs | — |
+| RTP header build | ~0.5 µs | ~0.5 µs | — |
+| FU fragmentation | ~1 µs | ~1 µs | — |
+| memcpy to ring slot | 0 | ~2 µs | +2 µs |
+| UDP sendmsg (×7 pkts) | ~35 µs | 0 | -35 µs |
+| Futex wake | 0 | 0 (steady) | — |
+| **Producer total** | **~37.5 µs** | **~4.5 µs** | **-33 µs** |
+
+The producer (venc) is **8× faster** per frame because it replaces 7×
+`sendmsg()` syscalls with 7× `memcpy` into the ring.
+
+Consumer side (wfb_tx) timing is identical to the standalone UDP case, except
+it reads from ring instead of recvmsg — saving the kernel socket buffer path.
+
+### End-to-end latency
+
+| Path | Hop latency |
+|------|-------------|
+| **UDP pipe**: venc → kernel → kernel → wfb_tx → kernel → NIC | 3 kernel crossings |
+| **SHM ring**: venc → ring → wfb_tx → kernel → NIC | 1 kernel crossing |
+
+---
+
+## 12. Implementation Plan
+
+### Phase 1: Ring buffer library (pure C99, no venc changes)
+
+1. Create `venc_ring.h` / `venc_ring.c` — SPSC ring with futex.
+2. Write host-side unit test (`test_venc_ring.c`) — concurrent producer/consumer.
+3. Verify on ARM (atomic ordering, cache line size).
+
+### Phase 2: venc SHM output mode
+
+1. Add `STREAM_MODE_SHM` enum.
+2. Add `shm://` URI parsing in config.
+3. Thread `ring` pointer through RTP call chain.
+4. Add SHM branch in `send_rtp_packet()`.
+5. Skip UDP socket creation for SHM mode.
+6. Update Makefile (ring sources always compiled — no conditional).
+
+### Phase 3: wfb_tx SHM input patch
+
+1. Add `--shm-input` CLI flag to `wfb_tx`.
+2. Replace `poll()` on UDP fd with `wfb_ring_read()` when SHM active.
+3. Keep control channel (tx_cmd UDP) unchanged.
+4. Create patch file for upstream submission or local maintenance.
+
+### Phase 4: tx_cmd integration
+
+1. Create `wfb_cmd.h` / `wfb_cmd.c` — command sender (pure C99).
+2. Add wfb field descriptors to `g_fields[]`.
+3. Add `apply_wfb_fec` / `apply_wfb_radio` callbacks.
+4. Wire through HTTP API with mode-aware visibility.
+5. Test live FEC/radio parameter changes during streaming.
+
+### Phase 5: Testing and validation
+
+1. Unit tests for ring buffer (host, ASAN, TSAN).
+2. Cross-compile and deploy both binaries to target.
+3. End-to-end: venc → SHM ring → wfb_tx → wfb_rx → decoder.
+4. Stress test: vary FPS, toggle output, pipeline reinit during streaming.
+5. Measure steady-state futex wake count (should be 0).
+6. Measure latency improvement vs UDP pipe.
+
+---
+
+## 13. Open Questions
+
+1. **Ring buffer sizing**: 256 slots × 1500 bytes (384 KB) is conservative.
+   Should this be configurable, or is a fixed size sufficient?
+
+2. **Consumer health monitoring**: Should venc monitor whether wfb_tx is
+   alive (e.g., check read_idx is advancing)?  If wfb_tx dies, should venc
+   fall back to UDP automatically?
+
+3. **Multiple consumers**: The SPSC design supports exactly one consumer.
+   If dual output is needed (wfb + recorder), should venc write to two
+   separate rings, or should the consumer forward to a second destination?
+
+4. **wfb_tx upstream acceptance**: Would the `--shm-input` patch be accepted
+   upstream by svpcom, or maintained as a local fork?  The patch is small
+   and self-contained.
+
+5. **tx_cmd port configuration**: Should the wfb_tx command port be part of
+   `venc.json` config (e.g., `"wfb_cmd_port": 9000`), or derived from a
+   convention (e.g., always `wfb_tx -u 9000`)?
