#!/usr/bin/env bash
#
# Test harness for video0.refPred (SVC-T reference pyramid).
#
# Confirms the feature works on hardware in three stages:
#
#   A. SDK accepted the call         (already proven by the "(applied)" log)
#   B. Bitstream structure differs   (temporal_id distribution changes)
#   C. Error resilience improves     (fewer decode errors under simulated loss)
#
# Routes the test capture to a separate UDP port so a live viewer on 5600
# is not disturbed.  Stops + starts the daemon between A/B configs because
# refPred is MUT_RESTART and SIGHUP does not re-read /etc/waybeam.json.

set -euo pipefail

BACKEND="${BACKEND:-star6e}"
DURATION="${DURATION:-30}"
TEST_PORT="${TEST_PORT:-5610}"
DEV_IP="${DEV_IP:-192.168.1.5}"
LOSS_RATES="${LOSS_RATES:-0.00 0.01 0.03 0.05}"
WORK_DIR="${WORK_DIR:-/tmp/refpred_test}"
SEED="${SEED:-42}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ANALYZE="${REPO_ROOT}/scripts/hevc_analyze.py"

case "${BACKEND}" in
  star6e) DEPLOY="${REPO_ROOT}/scripts/star6e_direct_deploy.sh" ;;
  maruko) DEPLOY="${REPO_ROOT}/scripts/maruko_direct_deploy.sh" ;;
  *) echo "Unknown backend: ${BACKEND}" >&2; exit 2 ;;
esac

log() { printf '\n=== %s ===\n' "$*"; }

mkdir -p "${WORK_DIR}"
SDP="${WORK_DIR}/stream.sdp"
cat > "${SDP}" <<EOF
v=0
o=- 0 0 IN IP4 0.0.0.0
s=Waybeam
c=IN IP4 0.0.0.0
t=0 0
m=video ${TEST_PORT} RTP/AVP 96
a=rtpmap:96 H265/90000
EOF

# Stash the current outgoing.server so we restore it on exit.
ORIG_SERVER="$(${DEPLOY} config-get .outgoing.server 2>/dev/null \
  | tail -n1 | sed -e 's/^[ \t]*//' -e 's/[ \t]*$//')"
TEST_SERVER='"udp://'"${DEV_IP}:${TEST_PORT}"'"'

cleanup() {
  if [ -n "${ORIG_SERVER:-}" ]; then
    log "Restoring outgoing.server -> ${ORIG_SERVER}"
    ${DEPLOY} config-set .outgoing.server "${ORIG_SERVER}" >/dev/null || true
    ${DEPLOY} reload >/dev/null || true
  fi
}
trap cleanup EXIT

capture_one() {
  local label="$1"
  local refBase="$2"
  local refEnhance="$3"
  local refPred="$4"
  local out="${WORK_DIR}/${label}.h265"

  log "${label}: refBase=${refBase} refEnhance=${refEnhance} refPred=${refPred}"

  # Push fresh default config to avoid json_cli append-outside-object bug,
  # then set the three refPred fields + outgoing.server to test port.
  scp -O -q "${REPO_ROOT}/config/waybeam.default.json" \
    "${DEPLOY_HOST}:/etc/waybeam.json"
  ${DEPLOY} config-set .video0.refBase "${refBase}" >/dev/null
  ${DEPLOY} config-set .video0.refEnhance "${refEnhance}" >/dev/null
  ${DEPLOY} config-set .video0.refPred "${refPred}" >/dev/null
  ${DEPLOY} config-set .outgoing.server "${TEST_SERVER}" >/dev/null

  ${DEPLOY} stop >/dev/null
  ${DEPLOY} start >/dev/null
  ${DEPLOY} wait-http >/dev/null

  # Confirm the running daemon has the value we set.
  local got_base
  got_base="$(${DEPLOY} config-get .video0.refBase 2>/dev/null | tail -n1 | tr -d '[:space:]')"
  echo "  daemon reports refBase=${got_base}"

  log "Capturing ${DURATION}s to ${out}"
  rm -f "${out}"
  timeout "$((DURATION + 5))" ffmpeg -loglevel error -y \
    -protocol_whitelist 'file,udp,rtp' \
    -i "${SDP}" \
    -t "${DURATION}" \
    -c:v copy -an "${out}" 2>&1 | tail -3 || true
  ls -la "${out}" || true
}

DEPLOY_HOST="$(${DEPLOY} --help 2>&1 | sed -n 's/.*Defaults to host \([^.]*\)\..*/\1/p' | head -1)"
DEPLOY_HOST="${DEPLOY_HOST:-root@192.168.1.13}"

log "Backend: ${BACKEND}  Deploy host: ${DEPLOY_HOST}  Test port: ${TEST_PORT}"
log "Work dir: ${WORK_DIR}"

# --- A/B capture -----------------------------------------------------------
capture_one "ref_off" 0 0 true
capture_one "ref_on"  1 4 true

# --- Stage B: bitstream structure -----------------------------------------
log "STAGE B: bitstream structure"
for label in ref_off ref_on; do
  echo
  echo "----- ${label} -----"
  python3 "${ANALYZE}" walk "${WORK_DIR}/${label}.h265"
done

# --- Stage C: loss A/B ----------------------------------------------------
log "STAGE C: loss simulation"
printf '%-12s %-8s %-10s %-12s %-12s\n' \
  "config" "loss" "frames_in" "errors" "concealments"

for label in ref_off ref_on; do
  for rate in ${LOSS_RATES}; do
    es="${WORK_DIR}/${label}.h265"
    err_log="${WORK_DIR}/${label}_${rate}.err"
    drop_log="${WORK_DIR}/${label}_${rate}.drop"
    python3 "${ANALYZE}" drop "${es}" "${rate}" --seed "${SEED}" 2> "${drop_log}" \
      | ffmpeg -loglevel error -y \
          -f hevc -i - -f null - 2> "${err_log}" || true

    # FFmpeg HEVC error patterns:
    #   "decode_slice_header error"
    #   "Could not find ref with POC"
    #   "missing picture"
    #   "concealing N missing reference frame(s)"
    errors=$(grep -cE "error|Error" "${err_log}" 2>/dev/null || echo 0)
    conceal=$(grep -cE "missing|concealing|Could not find ref" "${err_log}" \
      2>/dev/null || echo 0)
    drop_dropped=$(awk '{print $NF}' "${drop_log}" | tr -d '\n' || echo "")

    printf '%-12s %-8s %-10s %-12s %-12s\n' \
      "${label}" "${rate}" "-" "${errors}" "${conceal}"
  done
done

log "Done.  Artifacts in ${WORK_DIR}"
