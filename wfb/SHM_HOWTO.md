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

**Default: 0.5** (matches `-k 8 -n 12`, the recommended FEC config).

When M-bit fires on a partial block (say `actual_k=3` out of `fec_k=8`),
the emitter needs to decide how many parity packets to send:

```
parity_count = max(fec_k - actual_k, min(fec_n - fec_k, round(actual_k × ratio)))
```

- **Lower bound** `fec_k - actual_k` guarantees RX reaches its
  `has_fragments == fec_k` trigger and decodes the block.
- **Upper bound** `fec_n - fec_k` never exceeds the session's parity
  budget.
- **Target** `actual_k × ratio` scales proportional to the partial size.

### Picking `ratio`

Match it to `(n - k) / k` of your FEC config for **proportional
partial-block protection** — the same parity-to-data ratio as a full
block:

| `-k` / `-n` | `(n − k)/k` | Recommended `-r` | Behavior |
|---|---|---|---|
| `-k 8 -n 12` | 0.5 | **`-r 0.5`** (default) | Typical: ~33 % parity overhead |
| `-k 6 -n 18` | 2.0 | `-r 2.0` | Heavy FEC (200 % overhead, long-range) |
| `-k 10 -n 30` | 2.0 | `-r 2.0` | Heavy FEC |
| `-k 4 -n 12` | 2.0 | `-r 2.0` | Heavy FEC, smaller blocks |
| any | n/a | `-r 0` | Minimum-parity partial (zero margin, lowest CPU) |

The ratio is Q8.8 fixed-point internally; floats are accepted on the CLI
with resolution ≈ 0.004. Examples: `-r 0.5`, `-r 1.25`, `-r 2.0`.

The flag only affects **partial blocks** (when M-bit fires mid-block).
Full blocks always emit `n - k` parity regardless of `-r`.

### Performance impact (measured)

On SigmaStar Infinity6E, 25 Mbps / 120 fps H.265, `-k 8 -n 12`:

| Config | wfb_cpu | Tasklet rate /s | Wire overhead on partials |
|---|---|---|---|
| `-b 0` (no M-bit) | baseline | baseline | 0 (natural behaviour) |
| `-b 1 -r 0.5` (default) | ~+4 pp | ~+60 | +50 % wire per partial block |
| `-b 1 -r 0` | ~+3 pp | ~+40 | +30 % wire per partial block |
| `-b 1 -r 2.0` (over-protected) | ~+5 pp | ~+100 | +100 % wire per partial block |

`-r 0.5` is the sweet spot for `-k 8 -n 12` — proportional protection
with minimal overhead.

Toggle at runtime:

```bash
wfb_tx_cmd 8000 set_mbit -e 1 -r 0.5         # set both in one call
wfb_tx_cmd 8000 set_mbit -e 1 -r 0.25        # cut partial-block parity in half
```

### Sanity checks

- `-r 0` with `-b 1` → partial blocks get zero extra parity beyond the
  RX-trigger minimum. Any single packet loss on a partial block's wire
  is unrecoverable. Accepted but warn-worthy for bursty links.
- `-r > (n - k)` — saturates at `fec_n - fec_k`; no harm, no additional
  protection. Wasted CLI-side effort.
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
