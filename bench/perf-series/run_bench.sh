#!/usr/bin/env bash
#
# Perf-series bench runner.  Deploys the current build to the vehicle, runs
# the standard timing probe, collects per-thread syscall counters, and writes
# one result directory under bench/perf-series/results/<label>/.
#
# Call `bench/perf-series/compare.py baseline <label>` afterwards for the
# side-by-side Δ table.
#
# Usage:
#   bench/perf-series/run_bench.sh --label <pr-name> [options]
#
# Options:
#   --label NAME       result directory name (required)
#   --vehicle IP       default 192.168.1.13
#   --duration SEC     probe window in seconds (default 60; plus 2s slack)
#   --runs N           repeat the probe N times (default 3)
#   --hub-state STATE  "stopped" (default) or "running"
#   --perf-gov         pin CPU governor to 'performance' on vehicle
#   --skip-deploy      don't rebuild or scp; assume binary is live
#   --help
#
# Depends on:
#   tools/rtp_timing_probe   (host-native; built via `make rtp_timing_probe`)
#   ssh/scp key-based access to root@<vehicle>
#   json_cli on the vehicle

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
RESULTS_BASE="$SCRIPT_DIR/results"
PROBE_BIN="$REPO_ROOT/rtp_timing_probe"

LABEL=""
VEHICLE="${PERF_VEHICLE:-192.168.1.13}"
DURATION=60
RUNS=3
HUB_STATE="stopped"
PERF_GOV=0
SKIP_DEPLOY=0

show_usage() {
	sed -n '2,25p' "$0"
	exit 1
}

while [ $# -gt 0 ]; do
	case "$1" in
		--label)      LABEL="$2"; shift 2 ;;
		--vehicle)    VEHICLE="$2"; shift 2 ;;
		--duration)   DURATION="$2"; shift 2 ;;
		--runs)       RUNS="$2"; shift 2 ;;
		--hub-state)  HUB_STATE="$2"; shift 2 ;;
		--perf-gov)   PERF_GOV=1; shift ;;
		--skip-deploy) SKIP_DEPLOY=1; shift ;;
		--help|-h)    show_usage ;;
		*) echo "unknown option: $1" >&2; show_usage ;;
	esac
done

[ -n "$LABEL" ] || { echo "error: --label <name> is required" >&2; exit 2; }
case "$HUB_STATE" in stopped|running) ;; *) echo "bad --hub-state"; exit 2 ;; esac

RESULT_DIR="$RESULTS_BASE/$LABEL"
mkdir -p "$RESULT_DIR"

# ── Build probe if missing ───────────────────────────────────────────────
if [ ! -x "$PROBE_BIN" ]; then
	echo "[bench] building rtp_timing_probe"
	(cd "$REPO_ROOT" && make rtp_timing_probe >/dev/null)
fi

# ── Deploy binary ────────────────────────────────────────────────────────
if [ "$SKIP_DEPLOY" -eq 0 ]; then
	echo "[bench] building venc (star6e)"
	(cd "$REPO_ROOT" && make build SOC_BUILD=star6e >/dev/null)

	echo "[bench] stopping venc on $VEHICLE"
	ssh "root@$VEHICLE" 'killall venc 2>/dev/null; sleep 2; rm -f /tmp/venc.log' || true

	echo "[bench] deploying venc to $VEHICLE"
	scp -O -q "$REPO_ROOT/out/star6e/venc" "root@$VEHICLE:/usr/bin/venc"
fi

# ── Vehicle prep ─────────────────────────────────────────────────────────
if [ "$HUB_STATE" = "stopped" ]; then
	echo "[bench] stopping waybeam_hub"
	ssh "root@$VEHICLE" 'killall waybeam_hub 2>/dev/null; true' || true
fi

if [ "$PERF_GOV" -eq 1 ]; then
	echo "[bench] pinning governor to performance"
	ssh "root@$VEHICLE" 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done' || true
fi

echo "[bench] starting venc"
ssh "root@$VEHICLE" 'nohup venc > /tmp/venc.log 2>&1 &'
echo "[bench] waiting for pipeline init (15 s)"
sleep 15

# ── Metadata ─────────────────────────────────────────────────────────────
{
	echo "label=$LABEL"
	echo "vehicle=$VEHICLE"
	echo "duration=$DURATION"
	echo "runs=$RUNS"
	echo "hub_state=$HUB_STATE"
	echo "perf_gov=$PERF_GOV"
	echo "commit=$(cd "$REPO_ROOT" && git rev-parse HEAD)"
	echo "branch=$(cd "$REPO_ROOT" && git rev-parse --abbrev-ref HEAD)"
	echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULT_DIR/meta.txt"

# ── Probe runs ───────────────────────────────────────────────────────────
TIMEOUT=$((DURATION + 2))

for i in $(seq 1 "$RUNS"); do
	echo "[bench] run $i/$RUNS"

	# Capture venc PID + per-thread ctxt stats pre-run
	VENC_PID=$(ssh "root@$VEHICLE" 'pidof venc')
	ssh "root@$VEHICLE" "cat /proc/$VENC_PID/status" > "$RESULT_DIR/run${i}-status-pre.txt"
	ssh "root@$VEHICLE" "cat /proc/stat | head -2" > "$RESULT_DIR/run${i}-stat-pre.txt"

	timeout "$TIMEOUT" "$PROBE_BIN" --venc-ip "$VEHICLE" --stats \
		> "$RESULT_DIR/run${i}.tsv" 2> "$RESULT_DIR/run${i}.summary" || true

	ssh "root@$VEHICLE" "cat /proc/$VENC_PID/status" > "$RESULT_DIR/run${i}-status-post.txt" 2>/dev/null || true
	ssh "root@$VEHICLE" "cat /proc/stat | head -2" > "$RESULT_DIR/run${i}-stat-post.txt"
done

echo "[bench] wrote $RESULT_DIR"
echo "[bench] summarise with: bench/perf-series/compare.py <baseline> $LABEL"
