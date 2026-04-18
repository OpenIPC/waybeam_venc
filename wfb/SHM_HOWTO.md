# SHM Ring: Setup Guide for SigmaStar SoC

Zero-copy RTP packet transfer from venc to wfb_tx via POSIX shared memory,
eliminating UDP sendmsg/recvmsg kernel overhead on the video path.

**Target**: SSC30KQ / SSC338Q (SigmaStar Infinity6E), armv7l, OpenIPC Linux.

## Architecture

```
 waybeam_venc                   wfb_tx (patched)
 ┌──────────┐                  ┌──────────────┐
 │ H.265    │   /dev/shm/      │ FEC encode + │
 │ encoder  │──► venc_wfb ────►│ WiFi inject  │──► wlan0
 │ + RTP    │   (shared mem)   │              │
 └──────────┘                  └──────────────┘
     512 slots × 1412 bytes
     Lock-free SPSC ring, futex wake
```

No UDP sockets on the video path. Audio continues over UDP to localhost.

---

## Step 1: Build the patched wfb_tx

On the build host (x86_64 Linux):

```bash
cd wfb
./build_wfb_tx.sh
```

This will:
1. Download and cross-compile libsodium (static)
2. Clone wfb-ng from GitHub
3. Apply the SHM input patch (`-H` flag)
4. Cross-compile SHM diagnostic tools (`shm_ring_stats`, `shm_consumer_test`)
5. Produce all binaries in `wfb/build/`

Requires: the star6e toolchain at `../../toolchain/toolchain.sigmastar-infinity6e/`,
git, curl, autotools. First run downloads ~5 MB (libsodium) + ~2 MB (wfb-ng).

## Step 2: Deploy binaries to the SoC

```bash
# One-command deploy (builds if needed, then scp to device):
cd wfb
./build_wfb_tx.sh --deploy

# Or with a custom host:
DEPLOY_HOST=10.0.0.1 ./build_wfb_tx.sh --deploy

# Manual deploy:
scp -O wfb/build/{wfb_tx,wfb_keygen,shm_ring_stats,shm_consumer_test} \
    root@192.168.1.10:/usr/bin/
```

Note: `scp -O` is required for the SigmaStar dropbear SSH server.

## Step 3: Generate wfb-ng keys (first time only)

On the SoC:

```bash
cd /etc
wfb_keygen
# Creates gs.key (ground station) and drone.key (vehicle/drone)
```

Copy `gs.key` to your ground station (wfb_rx side).
Keep `drone.key` on the vehicle.

## Step 4: Configure venc for SHM output

Edit `/etc/venc.json` on the device (venc always loads from this path,
there is no `-c` flag). Back up first: `cp /etc/venc.json /etc/venc.json.bak`

Change the `outgoing` section:

```json
{
  "outgoing": {
    "enabled": true,
    "server": "shm://venc_wfb",
    "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "audioPort": 0
  }
}
```

Key settings:

| Field | Value | Notes |
|-------|-------|-------|
| `server` | `"shm://venc_wfb"` | The `venc_wfb` part becomes the SHM ring name (`/dev/shm/venc_wfb`) |
| `streamMode` | `"rtp"` | Required — SHM carries RTP packets |
| `maxPayloadSize` | `1400` | Each ring slot holds this + 12 bytes (RTP header) |
| `audioPort` | `0` | Disables audio (no UDP socket to share). Set to `5601` for audio over UDP to localhost |

### Audio in SHM mode

- `audioPort: 0` — audio disabled entirely (recommended if not needed)
- `audioPort: 5601` — audio sent via UDP to `127.0.0.1:5601`. You must run a
  separate `wfb_tx` instance for the audio stream (standard UDP input mode)

## Step 5: Start venc

```bash
# Start the encoder — it creates /dev/shm/venc_wfb automatically
venc &
```

Verify the ring was created:

```bash
ls -la /dev/shm/venc_wfb
# -rw------- 1 root root 725184 ... venc_wfb

shm_ring_stats venc_wfb
# slots=512 data_size=1412 total=725184
# write_idx=512 read_idx=0 lag=512
# ring FULL
```

The ring fills up immediately (FULL is expected without a consumer).
Once wfb_tx starts consuming, the lag drops to near zero.

## Step 6: Start wfb_tx with SHM input

```bash
# Typical usage — adjust -k/-n (FEC), -p (radio port), -B/-M (radio params)
wfb_tx -H venc_wfb \
       -K /etc/drone.key \
       -k 8 -n 12 \
       -p 0 \
       -B 20 -M 3 \
       -r 0.5 \
       wlan0
```

| Flag | Purpose |
|------|---------|
| `-H venc_wfb` | Read from SHM ring instead of UDP (the patch adds this) |
| `-K /etc/drone.key` | Encryption key |
| `-k 8 -n 12` | FEC: 8 data + 4 recovery packets |
| `-p 0` | Radio port (channel ID) |
| `-B 20 -M 3` | Bandwidth 20 MHz, MCS index 3 |
| `-r 0.5` | Partial-block parity ratio (matches `(n-k)/k=4/8=0.5`); see "Runtime tuning" below |
| `wlan0` | WiFi interface in monitor mode |

wfb_tx attaches to the ring, reads RTP packets via `venc_ring_read()`, and
injects them over WiFi. The control channel (FEC/radio commands via UDP)
continues working normally. If the SHM ring isn't available yet wfb_tx will
log `SHM ring 'venc_wfb' not available, waiting for producer...` and retry
every 500 ms instead of aborting — so start order is no longer strict.

Additional flags available from this patch (see full table in "Runtime
tuning"):

| Flag | Default | Purpose |
|------|---------|---------|
| `-b 0\|1` | `1` | M-bit FEC block close (frame-aligned blocks) |
| `-r RATIO` | `0.5` | Partial-block parity:data scaling |
| `-x` | off | Plaintext data fragments (video only — session still signed) |

All three are also runtime-tunable via `wfb_tx_cmd` (see below).

## Step 7: Verify

### Check ring is being consumed

```bash
shm_ring_stats venc_wfb
# write_idx=5678 read_idx=5670 lag=8
# ring ok
```

A small `lag` (< 50) means wfb_tx is keeping up. `lag=512` (ring FULL) means
wfb_tx can't keep up or isn't running.

### Measure throughput

```bash
# Run for 5 seconds (stop wfb_tx first to avoid contention)
shm_consumer_test venc_wfb 5
#   3383 pkt/s  23.8 Mbit/s
#   2593 pkt/s  20.8 Mbit/s
# === SHM Ring Consumer Results ===
# Duration:   2.5 s
# Packets:    7198 (2910 pkt/s)
# Data:       6.5 MB (22.0 Mbit/s)
# Avg pkt:    945 bytes
# Ring w_idx: 7218  r_idx: 7198  (lag: 20)
```

(Example output at 20 Mbps CBR, 720p120)

Note: `shm_consumer_test` competes with wfb_tx for packets. Stop wfb_tx
first if you want accurate throughput numbers, or use it only for quick
checks.

### Monitor wfb_tx output

wfb_tx logs statistics every second (configurable via `-l`):

```
SHM ring attached: 'venc_wfb' (512 slots x 1412 bytes)
SHM input mode: reading from ring 'venc_wfb'
```

## Startup order

Start order is **no longer strict** — wfb_tx retries attach every 500 ms
until the ring exists, and automatically reattaches if venc restarts (see
"Crashed producer" in Troubleshooting). Any order works; the sleep is
optional.

Recommended init script:

```bash
#!/bin/sh

# 1. Set up WiFi monitor mode
ip link set wlan0 down
iw dev wlan0 set type monitor
ip link set wlan0 up
iw dev wlan0 set channel 149 HT20

# 2. Start venc (creates the SHM ring)
# Config must already have server: "shm://venc_wfb" in /etc/venc.json
venc &

# 3. Start wfb_tx — attaches immediately if venc is up, else retries
wfb_tx -H venc_wfb -K /etc/drone.key -k 8 -n 12 -p 0 -B 20 -M 3 -r 0.5 wlan0 &
```

## Switching back to UDP mode

To revert to standard UDP output (no SHM), restore the original config:

```bash
cp /etc/venc.json.bak /etc/venc.json
# Or edit /etc/venc.json and change outgoing.server back to:
#   "server": "udp://192.168.2.20:5600"
```

Then restart venc and run wfb_tx with `-u` instead of `-H`:

```bash
killall venc; sleep 1; venc &
wfb_tx -u 5600 -K /etc/drone.key -k 8 -n 12 -p 0 wlan0
```

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `SHM ring '...' not available, waiting for producer` | venc not running or wrong name | Start venc; wfb_tx will attach automatically once the ring appears |
| `ring FULL` (lag=512) | wfb_tx not consuming fast enough | Check wfb_tx is running; reduce bitrate or FEC overhead |
| High lag but not full | Momentary burst; should recover | Normal during keyframes; monitor over time |
| `shm_ring_stats` shows `write_idx=0` | venc not encoding | Check venc logs; verify sensor/ISP init |
| `SHM producer stalled` | venc crashed without cleanup (watchdog kicked in at 3 s frozen write_idx) | wfb_tx auto-detaches; restart venc, wfb_tx will reattach |
| Audio not working | `audioPort=0` disables audio in SHM mode | Set `audioPort: 5601` and run separate `wfb_tx -u 5601` for audio |
| Video comes up but is black on ground | AEAD-mode mismatch (only one side has `-x`) | Check RX logs for `packets dropped (AEAD-mode mismatch, check -x flag on both sides)` — set or clear `-x` to match |
| `Invalid fragment_idx: N` at startup | Pre-session data packets (RX hasn't received session packet yet) — fixed in current build | None needed; the guard is automatic. Log should stop within 1 s |
| Segfault when running `wfb_tx` with no args | Upstream wfb-ng bug (not SHM patch) | Always pass a wlan interface. The unpatched binary has the same crash — `RawSocketTransmitter` constructor segfaults on empty wlans vector before the usage text prints |

## Ring parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Slot count | 512 | Power of 2, ~340 ms buffer at 30 fps/50 pkts per frame |
| Slot data size | maxPayloadSize + 12 | 1412 bytes default (1400 payload + 12 RTP header) |
| Total SHM size | ~724 KB | `sizeof(header) + 512 × stride` |
| Synchronization | Lock-free SPSC | `__atomic` acquire/release on indices |
| Consumer wake | Linux futex | Zero syscalls in steady state; futex only when ring empty |

---

# Runtime tuning: M-bit FEC close, parity ratio, cleartext mode

These features are layered on top of the base SHM input patch and work
for both SHM input (`-H`) and UDP input (`-u`).

## M-bit FEC block close (`-b`)

**Default: on.** When enabled, the RX M-bit (the last-packet-of-frame
marker in the H.264/H.265 RTP payload format) closes the current FEC
block at the frame boundary. Result: **one frame fits in one or more
complete FEC blocks, never spanning across two.** A block loss at the
boundary damages at most one frame instead of two.

- **On** (`-b 1`, default) — frame-aligned FEC. Slightly higher airtime
  on partial blocks; dramatically lower inter-frame loss correlation.
- **Off** (`-b 0`) — legacy wfb-ng behaviour. Blocks sized strictly by
  `k`; frames span block boundaries; single block loss can damage two
  adjacent frames.

Toggle at runtime:

```bash
wfb_tx_cmd 8000 set_mbit -e 0        # disable (legacy)
wfb_tx_cmd 8000 set_mbit -e 1        # enable (default)
wfb_tx_cmd 8000 get_mbit             # read back
```

## Partial-block parity ratio (`-r`)

**Default: auto-scale** — `-r` tracks `(n − k) / k` of the current FEC
config automatically unless you pass an explicit value on the CLI or
via `wfb_tx_cmd set_mbit -r`.  Auto-mode re-derives the ratio on every
`init_session()` / `CMD_SET_FEC`, so a dynamic controller calling
`set_fec k n` at runtime does not also need to push a matching
`set_mbit -r`.

### Mental model — two independent knobs

`-k` / `-n` and `-r` control FEC protection on different kinds of block,
and they **compose**:

| Block kind | Parity budget | Controlled by |
|---|---|---|
| **Full block** — `fec_k` data fragments filled before M-bit | **`n - k`** parity, always (unchanged from upstream wfb-ng) | `-k` / `-n` |
| **Partial block** — M-bit fires with `actual_k < fec_k` | `round(actual_k × r)` parity, capped at `n - k` | `-r` |

A single frame typically spans 0 or more full blocks + 1 partial block
(the tail, closed by the RTP M-bit marker). For example at 25 Mbps /
120 fps / MTU 1400 → ≈19 fragments per frame → 2 full blocks + 1 partial
per frame. At lower bitrates most frames are single partial blocks.

**To raise the steady-state FEC rate, lower `-k` or raise `-n`** — same
as pre-PR#10. `-r` is a second lever that only matters on the partial
tail block; it does not replace `-k/-n`.

### Wire layout of a partial block

```
parity_count   = min(n − k,  round(actual_k × r))
padding_wire   = max(0,      k − actual_k − parity_count)
wire_packets   = actual_k + padding_wire + parity_count
               = max(actual_k + parity_count,  fec_k)
```

Why the `fec_k` floor: RX's `apply_fec` trigger requires
`has_fragments == fec_k`. A block that reaches RX with fewer fragments
expires without decode and **all data is lost**. When `parity_count`
alone isn't enough (light-FEC configs like `-k 8 -n 12` where
`n − k = 4 < fec_k = 8`), 2-byte padding fragments fill the gap.

Wire **bytes**, however, are dominated by the parity packets (full MTU
each) not the padding (2 B each), so `-r` has a large effect on
byte-rate even when packet count stays constant. See "Worked examples"
below.

### Picking `-r`

Match it to `(n − k) / k` of your FEC config for **proportional
partial-block protection** — same parity-to-data ratio as a full block:

Auto-scale default (no `-r` on CLI) picks `(n − k)/k` automatically;
explicit overrides are listed below.  At very small `fec_k` the
computed value is floored at `1/256` so partials always emit ≥ 1
parity.

| `-k` / `-n` | `(n − k)/k` | Auto default | Full-block overhead | Partial-block behaviour |
|---|---|---|---|---|
| `-k 8 -n 12` | 0.5 | `-r 0.5` | 50 % parity | 1-2 parity + padding to `fec_k`; partial always emits 8 wire packets |
| `-k 6 -n 12` | 1.0 | `-r 1.0` | 100 % parity (2× coverage) | 3-6 parity; partial often emits `actual_k + parity_count` without padding |
| `-k 4 -n 12` | 2.0 | `-r 2.0` | 200 % parity | parity saturates at `n − k = 8`; long-range configs |
| `-k 2 -n 12` | 5.0 | `-r 5.0` | 500 % parity | parity saturates at `n − k = 10`; extreme FEC |
| `-k 6 -n 18` | 2.0 | `-r 2.0` | 200 % parity | heavy FEC |
| `-k 10 -n 30` | 2.0 | `-r 2.0` | same | heavy FEC |
| any | n/a | override with `-r 0` | (unchanged) | minimum-parity partial — zero margin on tail block |

The ratio is Q8.8 fixed-point internally; floats are accepted on the CLI
with resolution ≈ 0.004. Examples: `-r 0.5`, `-r 1.25`, `-r 2.0`.

### Auto-scale mode

When you start `wfb_tx` without `-r`, auto-scale is enabled and the
startup banner prints `parity_ratio=X.XXXX (auto)`.  Passing any `-r`
value flips auto off and the banner shows `(explicit)`.

At runtime:

```bash
wfb_tx_cmd 8000 get_mbit_auto        # read current mode (0=explicit, 1=auto)
wfb_tx_cmd 8000 set_mbit_auto -v 1   # enable auto (recomputes from current k/n)
wfb_tx_cmd 8000 set_mbit_auto -v 0   # freeze current ratio, stop tracking
wfb_tx_cmd 8000 set_mbit -r 0.25     # explicit ratio — also flips auto off
```

The lifecycle semantics:

| Starting state | Event | Result |
|---|---|---|
| auto=1 | `set_fec` | ratio recomputed from new `(n−k)/k`, auto stays 1 |
| auto=1 | `set_mbit -r V` | ratio = V, auto → 0 (explicit commit) |
| auto=0 | `set_fec` | ratio unchanged (operator kept control) |
| auto=0 | `set_mbit_auto -v 1` | auto → 1, ratio recomputed from current k/n |

The auto formula is `max(1, round((n − k) / k × 256))` in Q8.8.  The
`max(1, …)` floor prevents configs like `-k 4 -n 5` from rounding to
zero parity on very small `actual_k`.


### How to raise FEC coverage

Two levers, they compose:

| Goal | Knob | Effect |
|---|---|---|
| Raise **full-block** recovery capacity | lower `-k` or raise `-n` | applies to every full block; e.g. `-k 6 -n 12` doubles parity vs `-k 8 -n 12` |
| Raise **partial-block** recovery capacity | raise `-r` | applies only to the M-bit-closed tail; capped at `n − k` |
| Do both (max coverage, most airtime) | combine | e.g. `-k 6 -n 12 -r 1.0` — 1:1 parity:data on full AND partial |

### Tuning `-k` and `-r` to frame size

The optimal `-k` fits the **average frame** in one block (minus a small
headroom for natural variance).  When a frame exceeds `k × MTU`, it
spills into the next block — but M-bit close still aligns the tail, so
the spill is bounded and there's no cross-frame contamination.  The
partial tail's size is `actual_k = frame_frags mod fec_k` (plus 0 if
evenly divisible).

| Avg frame size (frags) | Example workload | Suggested `-k`/`-n` | Suggested `-r` | Rationale |
|---|---|---:|---:|---|
| **≤ 3** | low-rate stream: 4-6 Mbps @ 120 fps, or 2 Mbps @ 60 fps | `-k 4 -n 12` | `-r 2.0` | every frame is ~1 partial; `-r` carries all the FEC weight. Full-block overhead = 200 % but hits rarely |
| **4-8** | moderate: 8-15 Mbps @ 90-120 fps | `-k 6 -n 12` | `-r 1.0` | ~1 full + 0-1 partial per frame; 1:1 parity on both |
| **8-16** | typical FPV: 20-30 Mbps @ 60-120 fps | `-k 8 -n 12` (default) | `-r 0.5` (default) | 1-2 full blocks dominate wire; partial is the tail only |
| **> 16** | IDR bursts or very large P-frames (25+ Mbps) | same as 8-16 row | same | M-bit close bounds the tail; IDRs span 2-3 blocks without corrupting neighbours |

Two practical consequences:

1. **Choose `-k` first from the P-frame avg**, not the I-frame peak.
   I-frames are 3-10× larger but occur every GOP (0.3-2 s).  Sizing
   `-k` for the peak wastes airtime on every P-frame; sizing for the
   avg lets I-frames span a few blocks and amortises the cost.
2. **Then match `-r` to `(n − k) / k`** of the chosen config so partial
   parity tracks the full-block ratio — operator intuition is stable
   across loss scenarios.

### Future: dynamic `-k` via fec_controller

The `fec_controller` Python module (in this repo) is designed to pick
`-k` dynamically from an EWMA of observed frame sizes, bounded by a
learned headroom tracker.  Today it runs as a read-only observer; when
it's activated as an authoritative controller it will issue
`wfb_tx_cmd set_fec <k> <n>` calls as workload shifts.

**`-r` auto-tracks `-k` already** (enabled by default — see "Auto-scale
mode" above).  Once the controller is wired in, every `set_fec` it
issues will trigger a matching `(n − k)/k` recomputation on the
partial-block parity ratio — no additional `set_mbit` call from the
controller needed.  Operators who want an explicit, frozen `-r` can
still pass it on the CLI or flip auto off via
`wfb_tx_cmd set_mbit_auto -v 0`.

### Worked examples at `-k 8 -n 12`, typical partial `actual_k = 3`

| `-r` | `parity_count` | `padding_wire` | wire packets | wire bytes (approx) |
|---|---:|---:|---:|---:|
| 2.0 | 4 (capped at `n − k`) | 1 | 8 | 3·1400 + 2 + 4·1400 = ~9.8 KB |
| 1.0 | 3 | 2 | 8 | 3·1400 + 4 + 3·1400 = ~8.4 KB |
| 0.5 | 2 | 3 | 8 | 3·1400 + 6 + 2·1400 = ~7.0 KB |
| 0.25 | 1 | 4 | 8 | 3·1400 + 8 + 1·1400 = ~5.6 KB |

Packet count is **identical** across all four (the `fec_k = 8` floor);
byte count differs by ~1.75× between `-r 0.25` and `-r 2.0`. At light
FEC configs (`n − k < fec_k`), watch the **bytes-injected** counter in
the `PKT` line, not the packets-injected counter.

### Worked example at `-k 6 -n 12`, `actual_k = 3`

| `-r` | `parity_count` | `padding_wire` | wire packets |
|---|---:|---:|---:|
| 2.0 | 6 (capped at `n − k`) | 0 | 9 |
| 1.0 | 3 | 0 | 6 |
| 0.5 | 2 | 1 | 6 |

At `-k 6 -n 12`, `parity_max = 6 ≥ fec_k = 6`, so
`actual_k + parity_count ≥ fec_k` already and padding usually goes to
zero — wire packets track the ratio directly.

### Performance impact (measured)

On SigmaStar Infinity6E, 25 Mbps / 120 fps H.265, `-k 8 -n 12`:

| Config | wfb_cpu | Tasklet rate /s | Wire bytes on partials |
|---|---|---|---|
| `-b 0` (no M-bit close) | baseline | baseline | (no partials, tail spans next block) |
| `-b 1 -r 0.5` (default) | ~+4 pp | ~+60 | ~5× data per partial |
| `-b 1 -r 0` | ~+3 pp | ~+40 | ~3× data per partial |
| `-b 1 -r 2.0` (over-protected) | ~+5 pp | ~+100 | ~7× data per partial |

`-r 0.5` is the sweet spot for `-k 8 -n 12` — proportional protection
with minimal overhead.

Toggle at runtime:

```bash
wfb_tx_cmd 8000 set_mbit -e 1 -r 0.5         # set both in one call
wfb_tx_cmd 8000 set_mbit -e 1 -r 0.25        # cut partial-block parity in half
```

### Sanity checks

- `-r 0` with `-b 1` → partial blocks get zero extra parity beyond the
  RX-trigger minimum (achieved via padding at `-k 8 -n 12`, achievable
  via bare min at `-k 6 -n 12` or tighter). Any single packet loss on
  a partial block's wire is unrecoverable.
- `-r > (n − k) / actual_k` — saturates at `fec_n − fec_k`; no harm,
  no additional protection. Wasted CLI-side effort.
- Changing `-b` mid-session via `wfb_tx_cmd` is safe; RX doesn't need
  any knowledge of this setting (it's a TX-side emission policy only).

## Cleartext data mode (`-x`)

**Default: off (AEAD on).** Opt-in for one-way broadcast workloads
(consumer FPV video) where per-fragment ChaCha20-Poly1305 AEAD is
overhead without meaningful security benefit.

**Wire format change:**

- AEAD mode: `wblock_hdr_t` → AEAD(wpacket_hdr_t + payload) + 16 B tag
- Plain mode: `wblock_hdr_t` → wpacket_hdr_t + payload (no tag)

Both sides must match or packets are silently dropped. The
`count_p_mode_mismatch` stat on RX surfaces misconfigured deploys.

### What `-x` keeps

- **Session packet authentication.** fec_k, fec_n, epoch, channel_id are
  still protected by the pre-shared `tx.key`/`rx.key` keypair. An
  attacker can't inject a fake session packet to reconfigure RX.
- **Replay dedup.** RX's `count_p_uniq` set filters by
  `(block_idx, fragment_idx)` nonce, so exact replays are still
  rejected.

### What `-x` removes

- **Per-fragment authentication.** Anyone on the channel can inject
  fragments that RX accepts (subject to FEC decode succeeding).
- **Per-fragment integrity.** A single-bit flip that slips past FEC
  reaches the video decoder. For H.264/H.265 this is usually a glitch,
  not a crash.
- **Per-fragment confidentiality.** Video content visible to anyone in
  RF range.

### When `-x` makes sense

- One-way broadcast (video from drone to ground)
- Consumer/hobbyist use where opsec isn't load-bearing
- CPU/airtime matters (saves ~4 pp wfb_cpu + 16 B × pkt/s on wire)

### When to keep AEAD on

- **Bidirectional tunnel** (RC control, telemetry) — injected control
  packets are an actual attack vector
- **Commercial deploys** with compliance requirements
- Any scenario where integrity of the stream matters

### Usage

```bash
# TX side (vehicle)
wfb_tx -H venc_wfb -K /etc/drone.key -k 8 -n 12 -p 0 -r 0.5 -x wlan0

# RX side (ground station)
sudo wfb_rx -K /etc/gs.key -i 0 -p 0 -x wlxHHHHHHHHHHHH
```

No wfb_tx_cmd for this — switching mid-session would invalidate RX's
session-key state. Choose at startup.

### Verifying

TX startup logs:

```
Startup: AEAD=off (DATA_PLAIN), M-bit close=on, parity_ratio=0.5000, k=8, n=12, channel_id=0x0000cf
Note: RX must run with -x too; DATA_PLAIN packets are silently dropped in AEAD mode.
```

RX startup logs:

```
Startup: AEAD=off (expects DATA_PLAIN), channel_id=0x0000cf, rx_mode=local
Note: TX must run with -x too; AEAD DATA packets are silently dropped in plain mode.
```

If modes disagree, RX log will include per-interval:

```
N packets dropped (AEAD-mode mismatch, check -x flag on both sides)
```

## Quick reference — recommended FPV config

For a typical 25 Mbps H.265 FPV video stream (vehicle → ground, one-way):

```bash
# Vehicle
wfb_tx -K /etc/drone.key -M 5 -B 20 -k 8 -n 12 -P 1 -Q -S 1 -L 1 \
       -C 8000 -H venc_wfb -R 2097152 -s 2097152 -l 1000 -i 0 -p 0 \
       -r 0.5 -x wlan0

# Ground
sudo wfb_rx -K /etc/gs.key -i 0 -p 0 -x wlxHHHHHHHHHHHH

# Runtime tuning on vehicle (examples)
wfb_tx_cmd 8000 get_mbit                     # enable=1, parity_ratio=0.5000
wfb_tx_cmd 8000 set_mbit -e 1 -r 0.5         # proportional parity (default)
wfb_tx_cmd 8000 set_fec -k 10 -n 15          # change FEC live
```
