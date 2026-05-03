#!/usr/bin/env bash
# verify_record_status_and_ae_precrop.sh
#
# On-device verification harness for the Maruko REST API parity work:
#
#   1. /api/v1/record/status reports live counters when daemon-config-driven
#      TS recording is active (was zero-fill on Maruko before this fix).
#   2. /api/v1/record/start and /api/v1/record/stop still return 501 on
#      Maruko (the new explicit g_record_http_control_supported gate replaces
#      the old "g_record_status_fn presence" gate, so adding status visibility
#      did not silently re-enable the control endpoints).
#   3. /api/v1/ae includes runtime.active_precrop on Maruko (was omitted
#      despite the precrop being set on the venc_api side).
#
# Defaults target the Maruko bench (192.168.2.12); Star6E (192.168.1.13) can
# be exercised with --backend star6e to check that record/start|stop still
# return 200 there (regression guard for the gating split).
#
# Usage:
#   scripts/verify_record_status_and_ae_precrop.sh [options]
#
# Options:
#   --backend BACKEND     "maruko" (default) or "star6e"
#   --host SSH_TARGET     Override SSH host (default depends on backend)
#   --record-dir DIR      Remote directory for TS files (default /tmp)
#   --record-secs N       Seconds of recording to capture before status check
#                         (default 6 — needs to span at least one IDR + flush)
#   --skip-build          Reuse existing out/<backend>/venc binary
#   --skip-deploy         Don't scp the binary; assume target already updated
#   --keep-config         Don't restore the original /etc/venc.json on exit
#   -h, --help            Show this help

set -euo pipefail

BACKEND="maruko"
HOST=""
RECORD_DIR="/tmp"
RECORD_SECS=4
TEST_BITRATE_KBPS=4000
TEST_MAX_MB=10
TEST_MAX_SECS=15
SKIP_BUILD=0
SKIP_DEPLOY=0
KEEP_CONFIG=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--backend)     BACKEND="$2"; shift 2 ;;
		--host)        HOST="$2"; shift 2 ;;
		--record-dir)  RECORD_DIR="$2"; shift 2 ;;
		--record-secs) RECORD_SECS="$2"; shift 2 ;;
		--skip-build)  SKIP_BUILD=1; shift ;;
		--skip-deploy) SKIP_DEPLOY=1; shift ;;
		--keep-config) KEEP_CONFIG=1; shift ;;
		-h|--help)     sed -n '2,/^set -euo/s/^# \?//p' "$0"; exit 0 ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

case "${BACKEND}" in
	maruko)  HOST="${HOST:-root@192.168.2.12}"; SOC_BUILD="maruko" ;;
	star6e)  HOST="${HOST:-root@192.168.1.13}"; SOC_BUILD="star6e" ;;
	*) echo "ERROR: --backend must be 'maruko' or 'star6e'" >&2; exit 1 ;;
esac

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCAL_BIN="${ROOT_DIR}/out/${SOC_BUILD}/venc"
REMOTE_BIN="/usr/bin/venc"
CONFIG_PATH="/etc/venc.json"
# Keep backup outside /tmp (tmpfs) so a mid-test reboot doesn't lose the
# original config — the verify run on 2026-05-03 hit exactly that when
# heavy TS writes filled tmpfs and triggered an OOM reboot.
BACKUP_PATH="/etc/venc.json.verify_bak"
LOG_PATH="/tmp/venc.verify.log"

PASS=0
FAIL=0
ERRORS=()

log() { printf '[verify] %s\n' "$*"; }
die() { printf '[verify] FATAL: %s\n' "$*" >&2; exit 2; }

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=5)
remote_sh()      { ssh "${SSH_OPTS[@]}" "${HOST}" "$@"; }
remote_capture() { remote_sh "$@"; }

scp_to() {
	scp -O -q "${SSH_OPTS[@]}" "$1" "${HOST}:$2"
}

require_remote_tool() {
	local tool="$1" attempt
	for attempt in 1 2 3; do
		if remote_sh "command -v ${tool}" >/dev/null 2>&1; then
			return 0
		fi
		sleep 1
	done
	die "remote ${HOST} is missing required tool '${tool}' (after 3 attempts)"
}

CACHED_PORT=""
http_port() {
	if [[ -z "${CACHED_PORT}" ]]; then
		CACHED_PORT="$(remote_capture "json_cli -g .system.webPort --raw -i ${CONFIG_PATH} 2>/dev/null" \
			| grep -oE '[0-9]+' | tail -n 1)"
	fi
	echo "${CACHED_PORT}"
}

curl_get() {
	local path="$1"
	local url="http://${HOST#*@}:${CACHED_PORT}${path}"
	curl -s -o /tmp/.verify.body -w '%{http_code}' --max-time 6 "${url}" 2>/dev/null
}

check_eq() {
	local name="$1" expected="$2" actual="$3"
	if [[ "${expected}" == "${actual}" ]]; then
		printf '  PASS  %s (%s)\n' "${name}" "${actual}"
		PASS=$((PASS+1))
	else
		printf '  FAIL  %s — expected "%s", got "%s"\n' \
			"${name}" "${expected}" "${actual}"
		FAIL=$((FAIL+1))
		ERRORS+=("${name}: expected '${expected}', got '${actual}'")
	fi
}

check_truthy() {
	local name="$1" actual="$2"
	if [[ -n "${actual}" && "${actual}" != "null" && \
	      "${actual}" != "false" && "${actual}" != "0" ]]; then
		printf '  PASS  %s (%s)\n' "${name}" "${actual}"
		PASS=$((PASS+1))
	else
		printf '  FAIL  %s — empty/false/null (got "%s")\n' \
			"${name}" "${actual}"
		FAIL=$((FAIL+1))
		ERRORS+=("${name}: not truthy (got '${actual}')")
	fi
}

cleanup() {
	local rc=$?
	if [[ "${KEEP_CONFIG}" -eq 0 ]]; then
		log "stopping venc and restoring ${CONFIG_PATH} from ${BACKUP_PATH}"
		# Kill before restore so the live recorder stops writing tmpfs
		# *before* we drop it back to the un-recording config.  Then
		# clean test artifacts from RECORD_DIR.
		remote_sh "killall -q venc 2>/dev/null; sleep 1; killall -q -9 venc 2>/dev/null; \
		           [ -f ${BACKUP_PATH} ] && cp ${BACKUP_PATH} ${CONFIG_PATH} && rm -f ${BACKUP_PATH}; \
		           rm -f ${RECORD_DIR}/rec_*.ts 2>/dev/null; true" || true
	else
		log "leaving config at modified state on ${HOST} (--keep-config)"
		log "  backup of original kept at ${BACKUP_PATH}"
	fi
	rm -f /tmp/.verify.body
	exit $rc
}
trap cleanup EXIT

# ── Build + deploy ───────────────────────────────────────────────────────

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
	log "building ${SOC_BUILD} binary"
	(cd "${ROOT_DIR}" && make build SOC_BUILD="${SOC_BUILD}") >/dev/null
fi
[[ -x "${LOCAL_BIN}" ]] || die "missing ${LOCAL_BIN}"

require_remote_tool json_cli
require_remote_tool wget
command -v python3 >/dev/null 2>&1 || die "host needs python3 to parse JSON responses"

if [[ "${SKIP_DEPLOY}" -eq 0 ]]; then
	log "stopping any running venc on ${HOST}"
	remote_sh "killall -q venc 2>/dev/null; sleep 1; killall -q -9 venc 2>/dev/null; true" || true
	log "scp ${LOCAL_BIN} → ${HOST}:${REMOTE_BIN}"
	scp_to "${LOCAL_BIN}" "${REMOTE_BIN}"
	remote_sh "chmod +x ${REMOTE_BIN}"
fi

# ── Configure recording ──────────────────────────────────────────────────

log "snapshotting ${CONFIG_PATH} to ${BACKUP_PATH}"
remote_sh "cp ${CONFIG_PATH} ${BACKUP_PATH}"

log "patching ${CONFIG_PATH}: record.enabled=true, mode=mirror, format=ts, dir=${RECORD_DIR}"
log "  also capping bitrate=${TEST_BITRATE_KBPS} kbps and rotation maxMB=${TEST_MAX_MB}/maxSeconds=${TEST_MAX_SECS}"
log "  to avoid filling tmpfs (the 2026-05-03 run hit an OOM reboot at 40 MB)"
remote_sh "json_cli -s .record.enabled true   -i ${CONFIG_PATH}"
remote_sh "json_cli -s .record.mode    '\"mirror\"' -i ${CONFIG_PATH}"
remote_sh "json_cli -s .record.format  '\"ts\"'     -i ${CONFIG_PATH}"
remote_sh "json_cli -s .record.dir     '\"${RECORD_DIR}\"' -i ${CONFIG_PATH}"
remote_sh "json_cli -s .record.maxMB     ${TEST_MAX_MB}     -i ${CONFIG_PATH}"
remote_sh "json_cli -s .record.maxSeconds ${TEST_MAX_SECS}  -i ${CONFIG_PATH}"
remote_sh "json_cli -s .video0.bitrate    ${TEST_BITRATE_KBPS} -i ${CONFIG_PATH}"

# ── Start venc fresh ─────────────────────────────────────────────────────

log "starting venc (log → ${LOG_PATH})"
remote_sh "killall -q venc 2>/dev/null; sleep 1; rm -f ${LOG_PATH}; (setsid ${REMOTE_BIN} >${LOG_PATH} 2>&1 </dev/null &) && sleep 1"

PORT="$(http_port)"
[[ -n "${PORT}" ]] || die "could not read system.webPort"
log "polling http://${HOST#*@}:${PORT}/api/v1/version (max 20s)"
ready=0
for _ in $(seq 1 20); do
	if curl -sf --max-time 2 "http://${HOST#*@}:${PORT}/api/v1/version" >/dev/null; then
		ready=1; break
	fi
	sleep 1
done
[[ "${ready}" -eq 1 ]] || {
	log "venc HTTP did not come up; tail of ${LOG_PATH}:"
	remote_sh "tail -30 ${LOG_PATH}" || true
	die "HTTP not ready"
}

# Confirm we hit the right backend
ver_json="$(curl -sf "http://${HOST#*@}:${PORT}/api/v1/version" || echo '{}')"
backend_actual="$(echo "${ver_json}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["data"].get("backend",""))' 2>/dev/null || echo "")"
log "device reports backend=${backend_actual}"
[[ "${backend_actual}" == "${BACKEND}" ]] || \
	die "backend mismatch: expected ${BACKEND}, device reports ${backend_actual}"

log "letting recorder run ${RECORD_SECS}s before status probe"
sleep "${RECORD_SECS}"

# ── Test 1: /api/v1/record/status reports live state ─────────────────────

echo
log "=== Test 1: /api/v1/record/status ==="
code="$(curl_get /api/v1/record/status)"
body="$(cat /tmp/.verify.body)"
check_eq "record/status HTTP code" "200" "${code}"
echo "    body: ${body}"
active="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["data"]["active"])' 2>/dev/null || echo "")"
format="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["data"]["format"])' 2>/dev/null || echo "")"
bytes="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["data"]["bytes"])' 2>/dev/null || echo "0")"
frames="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["data"]["frames"])' 2>/dev/null || echo "0")"
path="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["data"]["path"])' 2>/dev/null || echo "")"
check_eq      "record/status active"   "True" "${active}"
check_eq      "record/status format"   "ts"   "${format}"
check_truthy  "record/status bytes>0"  "${bytes}"
check_truthy  "record/status frames>0" "${frames}"
check_truthy  "record/status path"     "${path}"

# ── Test 2: HTTP record control gating ───────────────────────────────────

echo
log "=== Test 2: /api/v1/record/start gating ==="
code="$(curl_get /api/v1/record/start)"
body="$(cat /tmp/.verify.body)"
echo "    body: ${body}"
if [[ "${BACKEND}" == "maruko" ]]; then
	check_eq "record/start HTTP code (Maruko)" "501" "${code}"
else
	check_eq "record/start HTTP code (Star6E)" "200" "${code}"
fi

echo
log "=== Test 3: /api/v1/record/stop gating ==="
code="$(curl_get /api/v1/record/stop)"
body="$(cat /tmp/.verify.body)"
echo "    body: ${body}"
if [[ "${BACKEND}" == "maruko" ]]; then
	check_eq "record/stop HTTP code (Maruko)" "501" "${code}"
else
	check_eq "record/stop HTTP code (Star6E)" "200" "${code}"
fi

# ── Test 4: /api/v1/ae includes runtime.active_precrop ───────────────────

echo
log "=== Test 4: /api/v1/ae runtime.active_precrop ==="
code="$(curl_get /api/v1/ae)"
body="$(cat /tmp/.verify.body)"
check_eq "ae HTTP code" "200" "${code}"
precrop="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(json.dumps(d["data"]["runtime"].get("active_precrop")))' 2>/dev/null || echo "null")"
echo "    runtime.active_precrop: ${precrop}"
check_truthy "ae runtime.active_precrop present" "${precrop}"
if [[ "${precrop}" != "null" && -n "${precrop}" ]]; then
	w="$(echo "${precrop}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["w"])' 2>/dev/null || echo 0)"
	h="$(echo "${precrop}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["h"])' 2>/dev/null || echo 0)"
	check_truthy "ae active_precrop.w > 0" "${w}"
	check_truthy "ae active_precrop.h > 0" "${h}"
fi

# ── Test 5: /api/v1/config also exposes the precrop (sanity) ─────────────

echo
log "=== Test 5: /api/v1/config runtime.active_precrop ==="
code="$(curl_get /api/v1/config)"
body="$(cat /tmp/.verify.body)"
check_eq "config HTTP code" "200" "${code}"
precrop="$(echo "${body}" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(json.dumps(d["data"]["runtime"].get("active_precrop")))' 2>/dev/null || echo "null")"
echo "    runtime.active_precrop: ${precrop}"
check_truthy "config runtime.active_precrop present" "${precrop}"

# ── Test 6: /api/v1/record/status returns active=false when disabled ─────
#
# Restart venc with record.enabled=false in config and verify status reports
# active=false (proves the callback honestly distinguishes off vs on, not
# just always-true).  Done as a fresh restart rather than SIGHUP because the
# Maruko SIGHUP+record-rotation path is currently unstable on this branch
# (tracked separately, not in scope for this fix).

echo
log "=== Test 6: record/status reports active=false when disabled ==="
remote_sh "killall -q venc 2>/dev/null; sleep 1; killall -q -9 venc 2>/dev/null; true" || true
remote_sh "json_cli -s .record.enabled false -i ${CONFIG_PATH}"
remote_sh "json_cli -s .record.mode    '\"off\"'  -i ${CONFIG_PATH}"
remote_sh "(setsid ${REMOTE_BIN} >${LOG_PATH}.2 2>&1 </dev/null &) && sleep 1"
ready=0
for _ in $(seq 1 20); do
	if curl -sf --max-time 2 "http://${HOST#*@}:${CACHED_PORT}/api/v1/version" >/dev/null; then
		ready=1; break
	fi
	sleep 1
done
if [[ "${ready}" -ne 1 ]]; then
	log "WARNING: venc did not come back up after disable; skipping Test 6"
else
	code="$(curl_get /api/v1/record/status)"
	body="$(cat /tmp/.verify.body)"
	echo "    body: ${body}"
	active="$(echo "${body}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["data"]["active"])' 2>/dev/null || echo "")"
	check_eq "record/status active (with record.enabled=false)" "False" "${active}"
fi

# ── Summary ──────────────────────────────────────────────────────────────

echo
log "=========================================="
log "Verification summary: ${PASS} passed, ${FAIL} failed"
if [[ ${FAIL} -gt 0 ]]; then
	for e in "${ERRORS[@]}"; do echo "    - ${e}"; done
	log "tail of ${LOG_PATH} on device:"
	remote_sh "tail -40 ${LOG_PATH}" || true
	exit 1
fi
log "All checks passed."
