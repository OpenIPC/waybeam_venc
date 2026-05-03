# Perf-series benchmarks

Harness for the Star6E perf-improvement series (PR-A through PR-E, landed
after the 2026-04-18 review).  Layered on top of `bench/README.md` (Tier
A/B/C methodology) — same vehicle, same probe, same procedure; this
directory just automates the deploy + probe + collect loop and writes a
stable on-disk format that `compare.py` can diff.

## Quick start

```bash
# 1. Tag the reference point (once, at start of the series):
git tag -a perf-series-baseline -m "Star6E perf series baseline"
git checkout perf-series-baseline
bench/perf-series/run_bench.sh --label baseline --perf-gov --runs 3

# 2. For each PR in the series:
git checkout feature/perf-series-<letter>
bench/perf-series/run_bench.sh --label <pr-name> --perf-gov --runs 3
bench/perf-series/compare.py baseline <pr-name>
```

## What the bench captures

`run_bench.sh` writes one directory per label under `results/<label>/`:

| File | Contents |
|---|---|
| `meta.txt` | commit, branch, vehicle, duration, hub state, governor, date |
| `run<N>.tsv` | per-frame rtp_timing_probe output (sender/probe timestamps) |
| `run<N>.summary` | probe stats block (encode / send spread percentiles) |
| `run<N>-status-{pre,post}.txt` | `/proc/<venc-pid>/status` — ctxt-switch deltas |
| `run<N>-stat-{pre,post}.txt` | `/proc/stat` — system-wide CPU / IRQ deltas |

`compare.py` parses the per-run summaries, averages across runs, and prints
a markdown Δ table with a regression flag (|Δ| > 1.5×σ on a lower-is-better
metric).

## Fixed conditions (the bench contract)

Match the Tier A/B/C methodology exactly — any deviation invalidates the
comparison against the checked-in baseline.

| Variable | Value |
|---|---|
| Vehicle | 192.168.1.13 (SSC338Q + IMX335) |
| Bitrate | 25 Mbps, persisted in `/etc/venc.json` |
| Sensor bin | `/etc/sensors/imx335_greg_fpvVII-gpt200.bin` |
| Codec | H.265 CBR, GOP 10, `qpDelta=-4`, `frameLost=true` |
| Governor | `performance` when `--perf-gov` is passed |
| Hub | stopped by default (cleanest signal; `--hub-state running` for realism) |
| Window | 60 s (+ 2 s slack for probe shutdown) × 3 runs |

## Merge gate

A PR is green to merge when `compare.py baseline <pr>` shows:

- No ⚠ regression flags, and
- Primary metric for the PR (stated in the plan) moves in the expected
  direction by at least 1×σ of the baseline.

Paste the full markdown table into the PR description.

## Files

- `run_bench.sh` — deploy + probe + collect
- `compare.py` — markdown Δ table
- `BASELINE.md` — checked-in baseline numbers for `perf-series-baseline`
  (frozen; update only if the baseline tag is re-cut)
- `results/` — per-run artifacts (gitignored except for the baseline run)
