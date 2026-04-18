#!/usr/bin/env bash
#
# idr_storm — fire N IDR requests at the venc HTTP API in under 1 s,
# then read /api/v1/stats before and after to show how many were honored.
#
# Validates the PR-B (IDR dedup / rate-limit) gate.  With the gate
# active and 100 ms min-spacing, 100 requests in 1 s should yield
# roughly 10 real IDRs (bounded by the spacing, not the request rate).
#
# Usage:
#   tools/idr_storm.sh [count] [host]
#   tools/idr_storm.sh 100 http://192.168.1.13

set -euo pipefail

COUNT="${1:-100}"
HOST="${2:-http://192.168.1.13}"
ENDPOINT_IDR="$HOST/request/idr"
ENDPOINT_STATS="$HOST/api/v1/idr/stats"

# Extract combined honored count across all channels from /api/v1/idr/stats.
# Returns 0 on any parse failure so the diff math still works.
idr_honored_total() {
	wget -q -O- "$ENDPOINT_STATS" 2>/dev/null | \
		grep -oE '"honored":[0-9]+' | \
		grep -oE '[0-9]+' | \
		awk '{sum+=$1} END {print sum+0}'
}

idr_dropped_total() {
	wget -q -O- "$ENDPOINT_STATS" 2>/dev/null | \
		grep -oE '"dropped":[0-9]+' | \
		grep -oE '[0-9]+' | \
		awk '{sum+=$1} END {print sum+0}'
}

echo "[idr_storm] target=$HOST count=$COUNT"
before_h=$(idr_honored_total)
before_d=$(idr_dropped_total)
echo "[idr_storm] honored=$before_h dropped=$before_d (before)"

start_ns=$(date +%s%N)
for i in $(seq 1 "$COUNT"); do
	wget -q -O- "$ENDPOINT_IDR" >/dev/null 2>&1 || true
done
end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

# Let any in-flight IDR register in stats.
sleep 1

after_h=$(idr_honored_total)
after_d=$(idr_dropped_total)
delta_h=$((after_h - before_h))
delta_d=$((after_d - before_d))

echo "[idr_storm] fired $COUNT in ${elapsed_ms} ms"
echo "[idr_storm] honored=$after_h dropped=$after_d (after)"
echo "[idr_storm] delta: +${delta_h} honored, +${delta_d} dropped"
if [ "$COUNT" -gt 0 ]; then
	awk "BEGIN{printf \"[idr_storm] ratio_honored=%.3f\\n\", $delta_h/$COUNT}"
fi

# Expected:
#   Pre-PR-B:  ratio_honored ≈ 1.0   (no gate; every request becomes an IDR)
#   Post-PR-B: ratio_honored ≪ 1.0   (rate-limited — ~10 in 1 s with 100 ms gate)
