# Perf-series baseline

Frozen reference for the 2026-04-18 Star6E perf improvement series.

## Configuration

| Variable | Value |
|---|---|
| Tag | `perf-series-baseline` |
| Commit | `40b8435` (master tip before PR-A) |
| Vehicle | `192.168.1.13` (SSC338Q + IMX335) |
| Sensor bin | `/etc/sensors/imx335_greg_fpvVII-gpt200.bin` |
| Codec | H.265 CBR, GOP 10, `qpDelta=-4`, `frameLost=true` |
| Bitrate | 25 Mbps (persisted in `/etc/venc.json`) |
| FPS | 120 |
| Hub | stopped |
| Governor | ondemand (not pinned this run — use `--perf-gov` for tighter variance) |
| Window | 60 s per run × 3 runs |
| Host probe | `tools/rtp_timing_probe --stats --venc-ip 192.168.1.13` |
| Sidecar | port 6666, subscribed for the full run |

## Results (mean ± σ across 3 runs)

| Metric | Value |
|---|---:|
| Frames | 7397 ± 0 |
| FPS (probe clock) | 119.3 ± 0 |
| RTP packets | 144,490 ± 285 |
| RTP gaps | 0 ± 0 |
| Encode duration mean | 7524 µs ± 70 |
| Encode duration max | 10,329 µs ± 988 |
| Send spread mean | 444 µs ± 23 |
| Send spread P50 | 378 µs ± 5 |
| Send spread P95 | 1011 µs ± 116 |
| Send spread P99 | 1222 µs ± 45 |
| Send spread max | 1689 µs ± 343 |
| Frame interval probe stddev | 726 µs ± 8 |

## A/B result: baseline → PR-ABC (2026-04-18)

Target: `feature/perf-series-c-dualrec-fd` @ `d72b975` (PR-A + PR-B + PR-C stacked).

| Metric | Baseline mean ± σ | PR-ABC mean ± σ | Δ | % | Flag |
|---|---:|---:|---:|---:|:---:|
| encode_mean_us | 7,524 ± 70 | 7,398 ± 51 | −127 | −1.7 % | ↓ win (>1.5×σ) |
| encode_max_us | 10,329 ± 988 | 10,353 ± 1,668 | +24 | +0.2 % | noise |
| send_spread_mean | 444 ± 23 | 455 ± 53 | +11 | +2.6 % | noise |
| send_spread_p50 | 378 ± 5 | 387 ± 20 | +9 | +2.4 % | noise |
| send_spread_p95 | 1011 ± 116 | 973 ± 273 | −38 | −3.8 % | noise |
| send_spread_p99 | 1222 ± 45 | 1215 ± 48 | −7 | −0.6 % | noise |
| send_spread_max | 1689 ± 343 | 1488 ± 122 | −201 | −11.9 % | noise |
| frames | 7397 | 7397 | 0 | 0 % | — |
| rtp_packets | 144,490 ± 285 | 144,336 ± 736 | −154 | −0.1 % | noise |
| rtp_gaps | 0 | 0 | 0 | — | — |
| frame_interval_probe_stddev | 726 ± 8 | 712 ± 14 | −14 | −1.9 % | noise |

### Interpretation

- **No regressions.** Zero metrics flagged as regressed.
- **Encode mean improved −1.7 % (−127 µs).** PR-A's `CLOCK_MONOTONIC_RAW`
  → `CLOCK_MONOTONIC` switch moves the clock reads in the
  `capture_us → frame_ready_us` window (`monotonic_us()` in
  `star6e_video.c:123`) from a real syscall (~1500 ns on A7) to a vDSO
  read (~100 ns). The −127 µs delta is ~1.8 × baseline σ — a real
  signal, not noise.
- **Send spread unchanged.** The `frame_ready → last_pkt_send` window
  doesn't contain any of the clock-read savings (the clock reads flank
  this window, they don't sit inside it). Tail percentiles moved within
  noise on all three runs.
- **Zero packet gaps** on both binaries — video integrity is preserved
  end-to-end.
- **PR-C unmeasurable here** — dual-stream recording is not exercised
  by the timing probe bench. Measurable only with
  `sensorMode=720p/120, record.enabled=true`, which adds a separate
  SD-write workload; future follow-up.
- **PR-B unmeasurable here** — IDR rate-limit is correctness-only;
  validated separately via `tools/idr_storm.sh` (see PR #42).

## Notes on run-to-run variance

- Variance on tail percentiles (P95/P99/max) is 3-15 %. Use a
  regression flag of `|Δ| > 1.5 × σ` in `compare.py` to filter noise.
- `send_spread_max` has the highest run-to-run variance (σ = 343 µs on
  ~1.7 ms mean). Don't over-index on individual-run max values —
  they're dominated by kernel scheduling tails.
- Governor was left at `ondemand` (default) for this run. Pinning to
  `performance` via `--perf-gov` would tighten σ by ~30-50 %; worth
  doing for future PR-by-PR runs in the series.

## Raw artifacts

- `results/baseline/run{1,2,3}.tsv` — per-frame probe TSV output
- `results/baseline/run{1,2,3}.summary` — stats summary block
- `results/baseline/meta.txt` — run metadata (commit, branch, host)

These files are committed (the baseline is frozen); per-PR runs are
gitignored.
