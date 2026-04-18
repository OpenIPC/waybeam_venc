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
ENDPOINT_IDR="$HOST/api/v1/dual/idr"
ENDPOINT_STATS="$HOST/api/v1/stats"

idr_count_in_stats() {
	# Accepts either JSON with "idr_count":<n> or a plaintext stats block.
	wget -q -O- "$ENDPOINT_STATS" 2>/dev/null | \
		grep -oE '"(idr_count|idr_inserted)"[[:space:]]*:[[:space:]]*[0-9]+' | \
		head -1 | \
		grep -oE '[0-9]+$' || echo 0
}

echo "[idr_storm] target=$HOST count=$COUNT"
before=$(idr_count_in_stats)
echo "[idr_storm] idr_count before=$before"

start_ns=$(date +%s%N)
# Fire COUNT requests in parallel-ish (serialized curl avoids mixed-up stats).
for i in $(seq 1 "$COUNT"); do
	wget -q -O- --post-data='' --method=POST "$ENDPOINT_IDR" >/dev/null 2>&1 || true
done
end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

# Let any in-flight IDR register in stats.
sleep 1

after=$(idr_count_in_stats)
honored=$((after - before))

echo "[idr_storm] fired $COUNT in ${elapsed_ms} ms"
echo "[idr_storm] idr_count after=$after  honored=$honored"
echo "[idr_storm] ratio=$(awk "BEGIN{print $honored/$COUNT}")"

# Expected:
#   Pre-PR-B:  honored ~= COUNT          (every request becomes an IDR)
#   Post-PR-B: honored << COUNT          (rate-limited by idr_min_spacing_ms)
