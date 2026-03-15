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
cd star6e-standalone/wfb
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
cd star6e-standalone/wfb
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
       wlan0
```

| Flag | Purpose |
|------|---------|
| `-H venc_wfb` | Read from SHM ring instead of UDP (the patch adds this) |
| `-K /etc/drone.key` | Encryption key |
| `-k 8 -n 12` | FEC: 8 data + 4 recovery packets |
| `-p 0` | Radio port (channel ID) |
| `-B 20 -M 3` | Bandwidth 20 MHz, MCS index 3 |
| `wlan0` | WiFi interface in monitor mode |

wfb_tx attaches to the ring, reads RTP packets via `venc_ring_read()`, and
injects them over WiFi. The control channel (FEC/radio commands via UDP)
continues working normally.

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

**venc must start before wfb_tx.** The SHM ring is created by venc
(`venc_ring_create`) and attached by wfb_tx (`venc_ring_attach`). If wfb_tx
starts first, it will fail with "Failed to attach to SHM ring".

Recommended init script order:

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
sleep 1  # wait for ring creation

# 3. Start wfb_tx (attaches to the ring)
wfb_tx -H venc_wfb -K /etc/drone.key -k 8 -n 12 -p 0 -B 20 -M 3 wlan0 &
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
| `Failed to attach to SHM ring` | venc not running or wrong name | Start venc first; check `ls /dev/shm/` |
| `ring FULL` (lag=512) | wfb_tx not consuming fast enough | Check wfb_tx is running; reduce bitrate or FEC overhead |
| High lag but not full | Momentary burst; should recover | Normal during keyframes; monitor over time |
| `shm_ring_stats` shows `write_idx=0` | venc not encoding | Check venc logs; verify sensor/ISP init |
| Audio not working | `audioPort=0` disables audio in SHM mode | Set `audioPort: 5601` and run separate `wfb_tx -u 5601` for audio |
| venc exits, ring disappears | `shm_unlink` on cleanup | wfb_tx will error on next read — restart both |
| Segfault when running `wfb_tx` with no args | Upstream wfb-ng bug (not SHM patch) | Always pass a wlan interface. The unpatched binary has the same crash — `RawSocketTransmitter` constructor segfaults on empty wlans vector before the usage text prints |

## Ring parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Slot count | 512 | Power of 2, ~340 ms buffer at 30 fps/50 pkts per frame |
| Slot data size | maxPayloadSize + 12 | 1412 bytes default (1400 payload + 12 RTP header) |
| Total SHM size | ~724 KB | `sizeof(header) + 512 × stride` |
| Synchronization | Lock-free SPSC | `__atomic` acquire/release on indices |
| Consumer wake | Linux futex | Zero syscalls in steady state; futex only when ring empty |
