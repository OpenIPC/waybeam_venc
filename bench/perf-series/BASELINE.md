# Perf-series baseline

**Placeholder** — populate this file with the numbers from the first
`run_bench.sh --label baseline` run against tag `perf-series-baseline`
(commit `40b8435`, master tip at the start of the series).

Run the bench as:

```bash
git checkout perf-series-baseline
bench/perf-series/run_bench.sh --label baseline --perf-gov --runs 3
```

Then paste the `run_bench.sh` output summary here plus the result of:

```bash
bench/perf-series/compare.py baseline baseline    # sanity self-diff (should show 0)
```

The checked-in baseline artifacts live under
`bench/perf-series/results/baseline/` and **must not be regenerated**
between PRs once the series starts — that would invalidate every Δ table
downstream.

## Expected baseline (from PR #35 final-tier-abc run, 2026-04-xx)

For reference while the real baseline bench hasn't been captured yet:

```
Frames:            7397 (119.3 fps)
RTP packets:       145566 (19.7 avg/frame)
Encode mean:       8014 us, max 10901 us
Send spread mean:  807 us, P50 778, P95 1307, P99 1663, max 2069 us
```

Reproduction of these numbers on `perf-series-baseline` is the first step
of the series.  Any deviation >5 % from these reference figures indicates
environmental drift (toolchain, kernel, sensor bin) and should be
investigated before running the series.
