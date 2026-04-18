#!/usr/bin/env python3
"""
Compare two perf-series result directories and emit a Δ table.

Usage:
    bench/perf-series/compare.py <baseline-label> <target-label>

Parses run*.summary (stderr of rtp_timing_probe --stats) under each label's
result directory, averages the numeric metrics across runs, and prints a
markdown table with mean, stdev, Δ, % change, and a flag for metrics where
|Δ| exceeds 1.5× stdev (likely real change, not noise).

No external deps — stdlib only.
"""

import math
import re
import sys
from pathlib import Path

METRICS = [
	# (display name, regex to capture a single float, "lower is better" flag)
	("encode_mean_us",     r"Encode duration.*?Mean:\s+(\d+)",          True),
	("encode_max_us",      r"Encode duration.*?Max:\s+(\d+)",           True),
	("send_spread_mean",   r"Send spread.*?Mean:\s+(\d+)",              True),
	("send_spread_p50",    r"Send spread.*?P50:\s+(\d+)",               True),
	("send_spread_p95",    r"Send spread.*?P95:\s+(\d+)",               True),
	("send_spread_p99",    r"Send spread.*?P99:\s+(\d+)",               True),
	("send_spread_max",    r"Send spread.*?Max:\s+(\d+)",               True),
	("frames",             r"Frames:\s+(\d+)",                          False),
	("fps",                r"Frames:\s+\d+\s+\(([\d.]+) fps",           False),
	("rtp_packets",        r"RTP packets:\s+(\d+)",                     False),
	("rtp_gaps",           r"RTP gaps:\s+(\d+)",                        True),
	("frame_interval_probe_stddev",
	                       r"Frame intervals \(probe clock\).*?Stddev:\s+(\d+)",
	                                                                    True),
]


def parse_summary(path: Path) -> dict:
	"""Return {metric_name: float} extracted from a .summary file."""
	text = path.read_text()
	# Collapse newlines so multi-line regex captures work without DOTALL.
	blob = " ".join(text.split())
	out = {}
	for name, rx, _ in METRICS:
		m = re.search(rx, blob)
		if m:
			out[name] = float(m.group(1))
	return out


def load_label(label: str) -> tuple[list[dict], dict]:
	d = Path(__file__).resolve().parent / "results" / label
	if not d.is_dir():
		raise SystemExit(f"no results dir: {d}")
	runs = sorted(d.glob("run*.summary"))
	if not runs:
		raise SystemExit(f"no runN.summary files under {d}")
	parsed = [parse_summary(p) for p in runs]
	meta_path = d / "meta.txt"
	meta = {}
	if meta_path.exists():
		for line in meta_path.read_text().splitlines():
			if "=" in line:
				k, v = line.split("=", 1)
				meta[k] = v
	return parsed, meta


def mean_stdev(xs: list[float]) -> tuple[float, float]:
	if not xs:
		return (float("nan"), float("nan"))
	m = sum(xs) / len(xs)
	if len(xs) < 2:
		return (m, 0.0)
	var = sum((x - m) ** 2 for x in xs) / (len(xs) - 1)
	return (m, math.sqrt(var))


def fmt(v: float) -> str:
	if math.isnan(v):
		return "—"
	if v >= 1000:
		return f"{v:,.0f}"
	if v >= 10:
		return f"{v:.1f}"
	return f"{v:.2f}"


def main(argv: list[str]) -> int:
	if len(argv) != 3:
		print(__doc__.strip(), file=sys.stderr)
		return 2
	base_label, targ_label = argv[1], argv[2]
	base_runs, base_meta = load_label(base_label)
	targ_runs, targ_meta = load_label(targ_label)

	print(f"# {base_label} → {targ_label}")
	print()
	for name, meta in [(base_label, base_meta), (targ_label, targ_meta)]:
		commit = meta.get("commit", "?")[:8]
		branch = meta.get("branch", "?")
		hub    = meta.get("hub_state", "?")
		dur    = meta.get("duration", "?")
		runs   = meta.get("runs", "?")
		print(f"- **{name}**: {branch} @ {commit}  "
		      f"(hub={hub}, duration={dur}s, runs={runs})")
	print()
	print("| Metric | Baseline mean ± σ | Target mean ± σ | Δ | % | Flag |")
	print("|---|---:|---:|---:|---:|:---:|")

	any_flag = False
	for name, _rx, lower_is_better in METRICS:
		b_vals = [r[name] for r in base_runs if name in r]
		t_vals = [r[name] for r in targ_runs if name in r]
		if not b_vals or not t_vals:
			continue
		b_m, b_s = mean_stdev(b_vals)
		t_m, t_s = mean_stdev(t_vals)
		delta = t_m - b_m
		pct   = (delta / b_m * 100.0) if b_m else float("nan")
		# Flag: |Δ| > 1.5 × max(b_s, t_s).  Direction indicator uses lower_is_better.
		noise = 1.5 * max(b_s, t_s)
		flag = ""
		if abs(delta) > noise and noise > 0:
			if (delta < 0 and lower_is_better) or (delta > 0 and not lower_is_better):
				flag = "↓ win"
			else:
				flag = "⚠ regression"
				any_flag = True
		elif noise == 0 and delta != 0:
			flag = "?"
		print(f"| {name} | {fmt(b_m)} ± {fmt(b_s)} | {fmt(t_m)} ± {fmt(t_s)} "
		      f"| {fmt(delta)} | {pct:+.1f}% | {flag} |")

	print()
	if any_flag:
		print("**Regression flag raised** — review ⚠ rows before merging.")
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
