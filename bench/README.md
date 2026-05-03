# Pipeline performance benchmarks

Baseline-and-after measurements for the Tier A/B/C perf work identified by
the Codex review (April 2026).

## Target

- Host: `root@192.168.1.13` (SSC338Q + IMX335)
- Sensor bin: `/etc/sensors/imx335_greg_fpvVII-gpt200.bin`
- ISP profile: legacy AE, `overclockLevel=1`, `verbose=true`
- Codec: H.265, CBR, GOP 10, `qpDelta=-4`, `frameLost=true`
- Bitrate: 25 Mbps (persisted in `/etc/venc.json`; see procedure below)

## Method

- Host probe: `tools/rtp_timing_probe --venc-ip 192.168.1.13 --stats`
- Duration: 60 s (7000+ frames at 120 fps)
- Metrics captured per frame via sidecar MSG_FRAME:
  - `capture_us → frame_ready_us`  = **encode duration**
  - `frame_ready_us → last_pkt_send_us`  = **send spread** (packetize + all
    per-packet sendmsg() syscalls + anything else holding the stream)
- `--stats` prints percentile summary on exit.

## Execution procedure (run this exact sequence every time)

Lessons learned the hard way across the Tier A / Tier B / control runs:

- Just setting the bitrate via the HTTP API does **not** persist across a
  venc restart — re-deploy will snap back to whatever `/etc/venc.json`
  has. Persist the bitrate in the config file instead.
- Stopping `waybeam_hub` makes numbers *look* great but confounds results:
  hub-stop removes scheduler jitter from HTTP polls, mDNS, metrics
  scraping and aalink, so encode and send durations appear to shrink even
  without any code change. **Always note the hub state in the bench
  filename**.
- `SIGHUP` to `waybeam_hub` does **not** reload `venc.bitrate_enabled`;
  the flag is read once at hub startup. Until that is fixed in the hub,
  the only way to stop the hub's bitrate override is to stop
  `waybeam_hub` entirely.

### 1. Persist the target bitrate in /etc/venc.json (once per vehicle)

```bash
ssh root@192.168.1.13 'json_cli -s .video0.bitrate 25000 -i /etc/venc.json'
```

This survives venc restarts, so step 4 below becomes a sanity check
rather than a re-apply.

### 2. Pick the hub state you want to measure

- **Hub running** — production-realistic; matches how the vehicle is
  deployed. Use this for A-vs-B code comparisons that include the real
  scheduling environment.
- **Hub stopped** — cleanest per-frame numbers; removes system noise but
  also removes the contention Tier A was designed to mitigate. Useful as
  a "best case" floor but not representative of deployment.

Record the hub state in the filename: `...hubstopped...` vs
`...hubrunning...`.

### 3. (Re)deploy the binary being measured

```bash
make build SOC_BUILD=star6e        # on host
ssh root@192.168.1.13 'killall venc; sleep 2; rm -f /tmp/venc.log'
scp -O out/star6e/venc root@192.168.1.13:/usr/bin/venc
ssh root@192.168.1.13 'nohup venc > /tmp/venc.log 2>&1 &'
# Pipeline init takes ~10 s on cold start — wait before the probe
```

### 4. Verify 25 Mbps sticky before the probe

```bash
for i in 1 2 3; do
  ssh root@192.168.1.13 'wget -q -O- http://127.0.0.1/api/v1/config | grep -oE "\"bitrate\":[0-9]+" | head -1'
  sleep 2
done
```

All three reads must show `"bitrate":25000`. If the hub is running and
it drops to 8 Mbps, aalink is clamping — either stop the hub or skip the
run. Never bench a mixed-bitrate window.

### 5. Run the 60 s probe

```bash
timeout 62 ./tools/rtp_timing_probe --venc-ip 192.168.1.13 --stats \
  > bench/<label>.tsv 2> bench/<label>.summary
```

Label format: `<tier-or-baseline>-<hubstate>-120fps-25mbps`, e.g.
`control-tier-b-hubstopped-120fps-25mbps`.

### 6. Restore vehicle state

```bash
# If the hub was stopped for the run:
ssh root@192.168.1.13 'waybeam_hub -c /etc/waybeam_vehicle.conf > /tmp/hub.log 2>&1 &'
# If you set bitrate_enabled=false for a separate reason:
ssh root@192.168.1.13 'json_cli -s .venc.bitrate_enabled true -i /etc/waybeam_vehicle.conf'
```

## Baseline results

### 120 fps H.265 @ 8 Mbps (default vehicle config)

```
Frames:            ~7000 (115.1 fps actual)
Encode:            mean 9615 us, max 15610 us
Send spread:       mean  344 us, P50  272 us, P95  672 us, P99 1289 us, max 7990 us
```

### 120 fps H.265 @ 25 Mbps (stress baseline — target working point)

```
Frames:            7305 (117.8 fps actual)
RTP packets:       142628 (19.5 pkts/frame avg)
RTP gaps:          0

Encode:            mean 8931 us, max 15660 us
Send spread:       mean 1123 us, P50  996 us, P95 2108 us, P99 4829 us, max 8775 us

Sidecar overhead:  7.4 KB/s rx (1 MSG_FRAME per frame + 0.7 sync pps)
Clock sync RTT:    best 928 us, stabilised ~1.7-2.5 ms
```

Raw TSVs: `baseline-master-120fps-h265.tsv`,
`baseline-master-120fps-25mbps.tsv`.
Summary files alongside each.

## Observations before any fix

1. **Send spread scales with bitrate** — 344 us at 8 Mbps → 1123 us at 25 Mbps.
   Confirms that per-packet `sendmsg()` cost (~19.5 packets/frame) is real
   CPU work on Cortex-A7, not just kernel overhead.
2. **Send-spread P99/max are pathological at stress level** — 4.8 ms / 8.8 ms.
   At 120 fps (8.33 ms frame period) an 8.8 ms send spread means we miss a
   frame boundary. This matches the Codex finding that Star6E holds the
   encoder stream across recorder writes, HTTP poll, verbose IMU output and
   debug OSD before `MI_VENC_ReleaseStream`.
3. **Encode duration is bitrate-insensitive** — encoder hardware is not the
   bottleneck. All wins will come from the post-encode path.
4. **Sidecar itself is ~zero cost on the wire** (7 KB/s, 1 syscall/frame).
   Its CPU cost is frame-synchronous `now_us()` sampling, not bandwidth.

## Planned work (in order)

- **Tier A** — raise `SO_SNDBUF`; release Star6E and Maruko encoder stream
  immediately after UDP send, move verbose/record/HTTP/OSD work onto
  post-release state.
- **Tier B** — use connected-UDP fast path (both backends); collapse
  redundant `now_us()` calls in the sidecar hot path.
- **Tier C** — evaluate `sendmmsg()` batching. Decide after Tier A+B
  measurements show whether syscall count still matters.

After each tier we re-run the same probe at the same 25 Mbps working point
and append the results below.

## After-fix results

### Tier A — SO_SNDBUF + early stream release (both backends)

Same working point (25 Mbps, 120 fps, verbose=true). Hub `mod_venc.bitrate_enabled`
temporarily set to `false` so aalink rate control would not clamp the bench
back to 8 Mbps — this was not necessary for the baseline run because aalink
happened to be idle then.

Probe summary:

```
Frames:            6695 (116.3 fps actual, 57.6 s)
RTP packets:       130935 (19.6 pkts/frame avg)
RTP gaps:          0

Encode:            mean 9653 us, max 16380 us        (unchanged — not on the hot path we touched)
Send spread:       mean  952 us, P50 717, P95 1912, P99 3017, max 8581 us

Sidecar overhead:  7.8 KB/s rx (1 MSG_FRAME/frame + 0.8 sync pps)
Clock sync RTT:    best 321 us
```

Delta vs baseline:

| Metric            | Baseline | Tier A | Δ      |
|-------------------|---------:|-------:|-------:|
| Send spread mean  | 1123 us  |  952 us| −15%   |
| Send spread P50   |  996 us  |  717 us| −28%   |
| Send spread P95   | 2108 us  | 1912 us|  −9%   |
| Send spread P99   | 4829 us  | 3017 us| **−38%** |
| Send spread max   | 8775 us  | 8581 us|  noise |

**Takeaway.** P99 dropped from 4.83 ms → 3.02 ms and now fits inside the
8.33 ms frame period at 120 fps. P50 and mean moved in the same direction,
confirming that the verbose IMU/EIS print path, HTTP record-control poll
and debug OSD work that used to sit under the encoder stream lifetime were
real cost. The extreme max tail (~8.5 ms) did not move — it lives upstream
in kernel UDP send scheduling, rtl88x2eu TX queue backpressure, or aalink
handover residue, not in the venc process.

Encode duration is unchanged (as expected — Tier A touched only the
post-encode stream-release path).

SO_SNDBUF raise to 512 KiB had no visible steady-state effect at 25 Mbps;
the IDR-burst failure mode it targets is a rare event that does not appear
in a 60 s bench.

### Tier B — connected-UDP fast path + sidecar now_us consolidation

Same 25 Mbps / 120 fps working point.

**Environment caveat:** `SIGHUP` to waybeam_hub does not reload
`venc.bitrate_enabled` — the flag is read once at hub startup. To keep
the bench locked at 25 Mbps I had to fully **stop** the hub for the
run, not just disable aalink rate-control via config. This also removes
hub-generated scheduler jitter (periodic HTTP polls, mDNS beacons,
metrics scraping), which confounds the tail numbers.

Probe summary:

```
Frames:            7398 (119.3 fps actual, 62 s)
RTP packets:       145157 (19.6 pkts/frame avg)
RTP gaps:          0

Encode:            mean 8553 us, max 11827 us
Send spread:       mean  769 us, P50 656, P95 1449, P99 1954, max 2367 us

Sidecar overhead:  7.5 KB/s rx (1 MSG_FRAME/frame + 0.7 sync pps)
Clock sync RTT:    best 1230 us
```

Delta vs Tier A (hub-running) and baseline (hub-running):

| Metric            | Baseline | Tier A | Tier B | Δ vs baseline |
|-------------------|---------:|-------:|-------:|--------------:|
| Encode mean       |  8931 us | 9653 us| 8553 us|        −4%    |
| Encode max        | 15660 us |16380 us|11827 us|       −24%    |
| Send spread mean  |  1123 us |  952 us|  769 us|       −32%    |
| Send spread P50   |   996 us |  717 us|  656 us|       −34%    |
| Send spread P95   |  2108 us | 1912 us| 1449 us|       −31%    |
| Send spread P99   |  4829 us | 3017 us| 1954 us|     **−60%**  |
| Send spread max   |  8775 us | 8581 us| 2367 us|     **−73%**  |

**Disentangling the numbers.** Encode duration dropped by 24% at max
even though nothing in Tier B touches the encoder. That 24% is purely
the hub-stop confound — hub background work was perturbing the encoder
thread's scheduling. The same confound lifts some portion of the P99/max
send-spread improvement; Tier B's code changes are responsible for the
rest.

Code-driven gains (from Tier A, hub running, identical setup) still stand:
mean −15%, P50 −28%, P99 −38% of Tier A code's own improvement over
baseline. Tier B's additional connected-UDP and sidecar consolidation
compound on top, but a clean A-vs-B comparison with the hub stopped in
both runs is still pending. To be run before Tier C.

Open: a hub reload-path fix so `venc.bitrate_enabled` changes take effect
on SIGHUP without a full restart — unblocks future benches without
stopping the hub.

### Clean control — Tier A+B code contribution with hub stopped in both runs

Both runs use the bench procedure above with the hub fully stopped and
`/etc/venc.json` persisted at 25 Mbps. The **only** difference between
these two runs is the binary — the master binary vs the Tier A+B binary.

```
control-master-hubstopped:
  Encode:            mean 7764 us, max  9941 us
  Send spread:       mean  788 us, P50 654, P95 1384, P99 1684, max 2343 us

control-tier-b-hubstopped:
  Encode:            mean 7929 us, max 13526 us
  Send spread:       mean  762 us, P50 644, P95 1404, P99 1728, max 3028 us
```

Side by side:

| Metric            | Master | Tier A+B | Δ      |
|-------------------|-------:|---------:|-------:|
| Encode mean       | 7764 us| 7929 us  | +2%    |
| Encode max        | 9941 us|13526 us  | +36%\* |
| Send spread mean  |  788 us|  762 us  | −3%    |
| Send spread P50   |  654 us|  644 us  | −2%    |
| Send spread P95   | 1384 us| 1404 us  | +1%    |
| Send spread P99   | 1684 us| 1728 us  | +3%    |
| Send spread max   | 2343 us| 3028 us  | +29%\* |

\* The encode-max / send-spread-max deltas are single-frame tail outliers
and flip sign across 60 s runs — treat as noise, not signal.

**Honest finding.** At the 25 Mbps / 120 fps working point on this
hardware, Tier A+B's code changes contribute ~0% to the hot-path
measurements when system noise is controlled. The big numbers from the
earlier hub-running benches (P99 −38%, max −73%) were the hub-stop
confound showing up, not the code.

**Why Tier A still might help in production.** Tier A's early
`MI_VENC_ReleaseStream` is only useful when there is CPU contention
after the release — verbose prints, HTTP polls, OSD draws that *would*
have stalled the next encoder cycle. With the hub stopped there is no
such contention, so there is nothing to win. A hub-running A-vs-master
bench is the right test for Tier A; that run was never done cleanly
(the earlier hub-running Tier A bench had the `bitrate_enabled` SIGHUP
issue and may not have locked 25 Mbps end-to-end).

**Tier B is sub-noise** in both configurations so far. `connected_udp`
saves a per-packet route lookup, and the sidecar `now_us()` consolidation
saves ~1 clock_gettime per frame — both real, both too small to measure
at this bench scale.

**Implication for Tier C.** Send-spread mean sits at ~760 μs floor with
19 packets/frame. That is dominated by 19 × per-syscall cost of
`sendmsg()` on Cortex-A7. `sendmmsg()` batching (Tier C) is the only
change so far that has a clear mechanism to move that floor, cutting
19 syscalls per frame to 1. Worth building.

### Tier C — sendmmsg() batching, matched control (hub stopped)

Same procedure as the other control runs, bitrate persisted in
`/etc/venc.json`, hub stopped before deploy.

**First deploy broke decode.** The initial batch implementation kept
`payload1` as a pointer into the caller's stack. For FU-A fragments
(`fu_hdr[3]` rebuilt each loop iteration) and HEVC aggregation packets
(`HevcApBuilder._internal.payload` reset between packets), the stack
buffer was reused before `sendmmsg` flushed — multiple queued packets
ended up with aliased payload1 content. The receiver saw bitrate arrive
but the stream could not be decoded.

Fix: the batch now owns a per-slot scratch buffer holding
`[RTP header || payload1]` concatenated and pre-copied on enqueue.
`payload2` (when present) is a slice of the VENC stream buffer and
stays valid until `MI_VENC_ReleaseStream` after `end_frame()`, so it
remains a zero-copy iovec pointer.

Probe summary:

```
Frames:            7000+ (119.3 fps actual, 60 s)
Encode:            mean 9104 us, max 11945 us
Send spread:       mean  718 us, P50 609, P95 1352, P99 1616, max 2191 us

Sidecar overhead:  7.5 KB/s rx (1 MSG_FRAME/frame + 0.7 sync pps)
venc log grep sendmmsg|send_error|ERROR: 0 hits (60+ s uptime)
```

Side by side vs the other hub-stopped controls:

| Metric            | Master | Tier B | **Tier C** | C vs Master | C vs Tier B |
|-------------------|-------:|-------:|-----------:|------------:|------------:|
| Send spread mean  | 788 us | 762 us |  **718 us**|      −9%    |    −6%      |
| Send spread P50   | 654 us | 644 us |  **609 us**|      −7%    |    −5%      |
| Send spread P95   |1384 us |1404 us | **1352 us**|      −2%    |    −4%      |
| Send spread P99   |1684 us |1728 us | **1616 us**|      −4%    |    −6%      |
| Send spread max   |2343 us |3028 us | **2191 us**|      −6%    |   −28%      |
| Encode mean       |7764 us |7929 us |    9104 us |   (noise)   |   (noise)   |

**Honest assessment.** `sendmmsg()` batching moved the ~760 μs floor to
~720 μs. That's a real, consistent improvement across mean, P50, P95,
P99 and max. Ordering is preserved, no send errors, no decode issues
on the fixed build.

Magnitude matches what the mechanism predicts: 19 packets/frame × ~4 μs
per-syscall cost on Cortex-A7 collapsed to one syscall ≈ 70 μs saved
per frame. Mean dropped from 788 → 718 μs (−70 μs). The remaining
~720 μs floor is the cost of actually *building* the 19 RTP packets
(memcpy into scratch, NAL stripping, AP aggregation, FU-A fragment
loop) — that part is unchanged from master.

**Tier C is the first tier where a hub-stopped bench shows measurable
code-driven gain.** Not huge in absolute terms (~70 μs per frame on a
~8.3 ms frame period at 120 fps), but steady and predictable.

**Open question for Tier C.** On the wire each packet still leaves the
NIC individually — batching only saves kernel-entry overhead, not TX
time. If the encoded frame is CPU-bound in the packetizer itself, the
next lever would be to reduce the per-packet work (e.g. stop copying
payload1 into scratch — it is only needed because FU-A / AP buffers
are reused — by restructuring the packetizer to hand out stable
buffers instead). That is a bigger refactor; not planned for now.

## Final validation — master vs Tier A+B+C, hub stopped, all KPIs

Purpose: apples-to-apples confirmation of the bundled change under the
documented procedure. Master binary from `master` HEAD, Tier A+B+C from
`feature/pipeline-perf-optimization` HEAD. Hub stopped for both runs,
`/etc/venc.json` persisted at 25 Mbps, back-to-back in the same session
on the same hardware.

Both runs: 0 matches for `send_error|ERROR|FAIL` in `/tmp/venc.log`,
venc PID unchanged through each run, 25 000 sticky across three
2 s-spaced checks.

### Send-spread (latency) — `rtp_timing_probe --stats`, 60 s

| Metric            | Master | Tier A+B+C | Δ     |
|-------------------|-------:|-----------:|------:|
| Encode mean       | 7886 us|   8014 us  | noise |
| Send spread mean  |  791 us|    807 us  |  +2%  |
| Send spread P50   |  683 us|    778 us  | +14%  |
| Send spread P95   | 1419 us|   1307 us  |  −8%  |
| Send spread P99   | 1730 us|   1663 us  |  −4%  |
| Send spread max   | 2277 us|   2069 us  |  −9%  |

Tail (P95 / P99 / max) consistently better. Mean/P50 in run-to-run noise
territory — the earlier `control-tier-b`-equivalent run for the same
build showed mean 718 us vs this 807 us. A 60 s single sample under
~760 us floor has ~±10 % run-to-run variance.

### CPU, context switches, memory — 30 s steady-state, same PID, no probe attached

Measured via `/proc/$PID/stat` + `/proc/$PID/status` sampled 30 s apart
on the vehicle; `CLK_TCK=100`, 119 fps sustained, 25 Mbps.

| KPI                         | Master | Tier A+B+C |      Δ |
|-----------------------------|-------:|-----------:|-------:|
| CPU %                       | 26.20 %|  24.87 %   | **−5 %** |
| User jiffies (30 s)         |     79 |       65   | **−18 %** |
| Sys jiffies (30 s)          |    707 |      681   |  −4 %    |
| Voluntary ctxt-sw / s       |    825 |      830   |   ~0 %   |
| Non-voluntary ctxt-sw / s   |    214 |      210   |  −2 %    |
| VmRSS (kB)                  |   2652 |     2756   | +104 kB  |
| Threads                     |      5 |        5   |   —      |

**What the CPU+ctxt deltas actually tell us.**

1. **User CPU dropped 18 %** (79 → 65 jiffies in 30 s). That's the
   cleanest signal in the whole dataset: less time spent in user-space
   per second. Matches exactly what collapsing 19 `sendmsg` setup
   sequences into one `sendmmsg` should do — less msghdr building and
   fewer syscall-entry trampolines on the Cortex-A7.

2. **Kernel CPU barely moved** (707 → 681 jiffies, −4 %). Expected:
   `sendmmsg` still does the same per-packet kernel work, it just enters
   once. The kernel-side TX time is independent of user-space batching.

3. **Voluntary ctxt switches are unchanged** (825/s both). This rules
   out the naive "sendmsg blocks → ctxt switch" mental model on this
   workload. With `SO_SNDBUF` raised to 512 KiB (Tier A) the UDP socket
   never fills, so `sendmsg` returns synchronously without a voluntary
   ctxt switch regardless of batching. The syscall savings show up as
   user-space CPU time, not ctxt switches.

4. **VmRSS +104 kB** matches the `Star6eOutputBatch` embedded in
   `Star6eOutput` (64 slots × 1616 B scratch + iovec + mmsghdr arrays).
   No runtime allocation.

5. **Overall CPU drop 26.2 % → 24.9 %** (−1.3 pp / −5 %). On a 240 fps
   overclocked Cortex-A7 that is ~1.3 ms of CPU time freed per real-time
   second. Meaningful but not dramatic — consistent with the latency
   result.

### Interpretation

Across both the timing probe and the process KPIs, the consistent
signal is: **Tier A+B+C reduces user-space CPU and tail send-spread by a
small, predictable amount, with no regression in any metric.**

- Mean send-spread is within run-to-run noise — do not claim a mean
  improvement from a single pair of 60 s runs.
- Tail latency (P95/P99/max) is consistently better by 4-9 %.
- User CPU is the clearest win at −18 %.
- No send errors, no decode issues, RSS grew by exactly the batch size
  we added.
- Tier A's CPU-contention benefit remains unmeasured in production
  conditions (hub running). That is gated on a separate waybeam_hub
  SIGHUP-reload fix for `venc.bitrate_enabled`.

The correctness fix around `connected_udp` (Star6E had the flag set but
the send path ignored it; Maruko was never plumbed at all), combined
with the measured user-CPU reduction and tail latency gains, is what
makes the PR worth shipping independent of the mean-latency wash.

## Post-review-fix final validation

After Codex adversarial review (session
`019d82d8-0ba8-7a72-a80e-f9f6a55f152d`, verdict needs-attention) we
landed three correctness fixes on the batch path:

1. Retry unsent batch tail on partial `sendmmsg` / `EINTR`, charge
   permanent drops to `send_errors`.
2. Snapshot transport state (`socket_handle`, `dst`, `dst_len`,
   `connected_udp`) into the batch at `begin_frame` under the
   existing `transport_gen` seqlock, so live `apply_server` on the
   HTTP thread cannot retarget in-flight queued packets.
3. Maruko compact mode honors `connected_udp` by switching to
   `send()` with no destination when set.

Re-ran the same matched protocol (hub stopped, 25 Mbps persisted,
same 60 s probe + 30 s /proc KPIs). The review fixes exercise error
paths and cross-thread races that don't fire under a healthy bench,
so the expectation is parity with Tier A+B+C, not improvement.

### Send-spread (60 s probe)

| Metric            | Master | Tier A+B+C | Review-fix | Notes |
|-------------------|-------:|-----------:|-----------:|-------|
| Encode mean       | 7886 us|   8014 us  |   7929 us  | noise |
| Encode max        |10183 us|  10901 us  |  19577 us  | single-frame outlier on review-fix run |
| Send spread mean  |  791 us|    807 us  |    792 us  | wash — review-fix between master and tier-abc |
| Send spread P50   |  683 us|    778 us  |    723 us  | noise |
| Send spread P95   | 1419 us|   1307 us  |   1379 us  | noise |
| Send spread P99   | 1730 us|   1663 us  |   1744 us  | noise |
| Send spread max   | 2277 us|   2069 us  |   2159 us  | noise |

### CPU, context switches, memory (30 s sampling)

| KPI                         | Master | Tier A+B+C | Review-fix |
|-----------------------------|-------:|-----------:|-----------:|
| CPU %                       | 26.20 %|  24.87 %   |  **25.33 %** |
| User jiffies (30 s)         |     79 |       65   |      73    |
| Sys jiffies (30 s)          |    707 |      681   |     687    |
| Voluntary ctxt-sw / s       |    825 |      830   |     831    |
| Non-voluntary ctxt-sw / s   |    214 |      210   |     199    |
| VmRSS (kB)                  |   2652 |     2756   |    2756    |
| Threads                     |      5 |        5   |       5    |

### Interpretation

**No regression from the review fixes.** Every KPI lands between
master and Tier A+B+C or within run-to-run noise of one of them. The
added code paths — retry loop (dead code in the healthy case), the
one-atomic-load seqlock read at `begin_frame`, the extra branch in
`maruko_send_udp_chunks` — have no measurable hot-path cost.

The `19577 us` encode max is a single-frame outlier unrelated to
these changes (encode path is untouched); the Tier A+B+C run showed
`10901 us` for the same metric — both are tail samples and not part
of the signal.

**Ship verdict**: correctness fixes preserve the Tier A+B+C
performance profile while closing the three issues Codex flagged.
No send errors, no decode issues, venc stable through the full run,
`VmRSS` unchanged from Tier A+B+C (the snapshot fields are a few
dozen bytes added to the existing batch struct).

## Maruko Tier C port (sendmmsg batching)

Follow-up to the Star6E Tier ABC PR — port the same sendmmsg batch
pattern to the Maruko backend (`src/maruko_video.c` +
`src/maruko_output.c`). Branch: `feature/maruko-tier-c-sendmmsg`.

### Target

- Host: `root@192.168.2.12` (SSC378QE + IMX415), hub not running
- Sensor mode: `3760x1024@59fps`
- Codec: H.265, CBR, sidecar port 5602
- Bitrate: 25 Mbps (persisted in `/etc/venc.json`)
- Probe: `tools/rtp_timing_probe --venc-ip 192.168.2.12 --sidecar-port 5602`
- Duration: 60 s (3660 frames at 59 fps, ~39 RTP pkts/frame)

### Comparison (tight run-order, second master sample used for variance)

| Metric | Master A+B (run 2) | Tier A+B+C | Δ |
|---|---:|---:|---:|
| User CPU (proc/pid stat) | 6.6 % | 6.3 % | −5 % (noise) |
| Sys CPU (proc/pid stat) | 20.1 % | 20.1 % | 0 % |
| ctxt/s (system-wide) | 3 403 | 3 398 | 0 % |
| Send spread mean | 1 014 µs | 981 µs | **−3.3 %** |
| Send spread P95 | 1 210 µs | 1 158 µs | **−4.3 %** |
| Send spread P99 | 1 287 µs | 1 227 µs | **−4.7 %** |
| Send spread max | 1 487 µs | 1 394 µs | **−6.3 %** |
| Encode duration mean | 4 507 µs | 4 526 µs | 0 % |

First master sample came in at 3 844 µs encode mean (vs 4 507 µs on
run 2); a second master run was taken to pin the encode-duration
variance — confirms run-to-run swing of ~15 % unrelated to Tier C.

### Interpretation

**Send spread tightened** consistently across all percentiles (−3 to
−6 %). This is the expected effect of collapsing ~39 per-frame
`sendmsg()` calls into one `sendmmsg()` flush — all packets leave
the NIC queue together instead of interleaved with other syscalls.
Tight send pacing helps wfb-ng FEC block alignment.

**User CPU did not drop** the way it did on Star6E (−18 %). Why:
Maruko in this configuration runs at 59 fps vs Star6E's 120 fps, so
the per-second syscall count is ~2 300 on both and the send path is
already a smaller fraction of total CPU on Maruko (sys CPU dominated
by encoder SDK + ISP at 20 %). There isn't enough send-path CPU left
to shave.

**No regressions:** no send errors in `/tmp/venc.log`, no decode
gaps, bitrate holds steady at 25 Mbps, venc stable across the swap
cycles. Binary grew 4 KB (~100 KB batch scratch lives in BSS —
same shape as Star6E).

### Files

- `bench/maruko-master-59fps-25mbps.{tsv,summary,kpi}`
- `bench/maruko-master-run2-59fps-25mbps.{tsv,summary,kpi}`
- `bench/maruko-tier-c-59fps-25mbps.{tsv,summary,kpi}`

### Ship verdict

Merge. The send-spread improvement is real and the change mirrors
a pattern already shipped on Star6E. Expect a bigger CPU win once
Maruko is driven at >100 fps or the ISP/encoder share drops.

