#!/usr/bin/env bash
# verify_record_status_and_ae_precrop.sh
#
# Maruko on-device verification for two REST parity fixes:
#   1. /api/v1/record/status reports live counters when daemon-config-driven
#      TS recording is active.
#   2. /api/v1/record/start and /stop still return 501 (the gating split
#      did not silently re-enable HTTP-driven control).
#   3. /api/v1/ae includes runtime.active_precrop.
#
# Star6E is verified by direct curl in the PR description, not by this
# harness — Star6E was already working and the only check there is "didn't
# regress to 501."
#
# WARNING: writes TS files to /tmp on the device.  Maruko's /tmp is
# unbounded tmpfs and an unrestricted recording will OOM-reboot it; this
# script caps bitrate=4 Mbps and rotation maxMB=10.
#
# Usage: scripts/verify_record_status_and_ae_precrop.sh [HOST]
#   HOST defaults to root@192.168.2.12

set -euo pipefail

HOST="${1:-root@192.168.2.12}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCAL_BIN="${ROOT_DIR}/out/maruko/venc"
CFG="/etc/venc.json"
BAK="/etc/venc.json.verify_bak"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15)
PASS=0; FAIL=0
HOST_IP="${HOST#*@}"

log()    { printf '[verify] %s\n' "$*"; }
rsh()    { ssh "${SSH_OPTS[@]}" "${HOST}" "$@"; }
cleanup() {
	log "restoring config and stopping venc"
	rsh "killall -q venc 2>/dev/null; sleep 1; killall -q -9 venc 2>/dev/null; \
	     [ -f ${BAK} ] && cp ${BAK} ${CFG} && rm -f ${BAK}; \
	     rm -f /tmp/rec_*.ts; true" || true
}
trap cleanup EXIT

check() {
	local name="$1" expected="$2" actual="$3"
	if [[ "${expected}" == "${actual}" ]]; then
		printf '  PASS  %s (%s)\n' "${name}" "${actual}"; PASS=$((PASS+1))
	else
		printf '  FAIL  %s — expected %s, got %s\n' "${name}" "${expected}" "${actual}"; FAIL=$((FAIL+1))
	fi
}

[[ -x "${LOCAL_BIN}" ]] || { log "missing ${LOCAL_BIN} — run: make build SOC_BUILD=maruko"; exit 1; }

log "deploying ${LOCAL_BIN} to ${HOST}"
rsh "killall -q venc 2>/dev/null; sleep 1; killall -q -9 venc 2>/dev/null; true" || true
scp -O -q "${SSH_OPTS[@]}" "${LOCAL_BIN}" "${HOST}:/usr/bin/venc"

log "patching ${CFG} (record on, bitrate 4 Mbps, rotation maxMB=10)"
rsh "cp ${CFG} ${BAK}"
rsh "json_cli -s .record.enabled true       -i ${CFG}; \
     json_cli -s .record.mode    '\"mirror\"' -i ${CFG}; \
     json_cli -s .record.format  '\"ts\"'     -i ${CFG}; \
     json_cli -s .record.dir     '\"/tmp\"'   -i ${CFG}; \
     json_cli -s .record.maxMB     10        -i ${CFG}; \
     json_cli -s .video0.bitrate   4000      -i ${CFG}"

log "starting venc"
rsh "(setsid /usr/bin/venc >/tmp/venc.verify.log 2>&1 </dev/null &); sleep 1"
for _ in $(seq 1 20); do
	curl -sf --max-time 2 "http://${HOST_IP}/api/v1/version" >/dev/null && break
	sleep 1
done
log "letting recorder run 4s"
sleep 4

log "=== /api/v1/record/status ==="
body="$(curl -s --max-time 5 "http://${HOST_IP}/api/v1/record/status")"
echo "  ${body}"
check "active"  "true"  "$(echo "${body}" | python3 -c 'import sys,json;print(str(json.load(sys.stdin)["data"]["active"]).lower())')"
check "format"  "ts"    "$(echo "${body}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["data"]["format"])')"
frames="$(echo "${body}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["data"]["frames"])')"
[[ "${frames}" -gt 0 ]] && { printf '  PASS  frames > 0 (%s)\n' "${frames}"; PASS=$((PASS+1)); } || { printf '  FAIL  frames == 0\n'; FAIL=$((FAIL+1)); }

log "=== /api/v1/record/{start,stop} (Maruko: expect 501) ==="
for ep in start stop; do
	code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://${HOST_IP}/api/v1/record/${ep}")"
	check "record/${ep}" "501" "${code}"
done

log "=== /api/v1/ae runtime.active_precrop ==="
body="$(curl -s --max-time 5 "http://${HOST_IP}/api/v1/ae")"
precrop="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(json.dumps(d["data"]["runtime"].get("active_precrop")))')"
echo "  ${precrop}"
[[ "${precrop}" != "null" ]] && { printf '  PASS  precrop present\n'; PASS=$((PASS+1)); } || { printf '  FAIL  precrop missing\n'; FAIL=$((FAIL+1)); }

echo
log "summary: ${PASS} passed, ${FAIL} failed"
[[ ${FAIL} -eq 0 ]] || exit 1
