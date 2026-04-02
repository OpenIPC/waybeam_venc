#!/usr/bin/env bash
set -euo pipefail

# FPS Sweep Test Harness for Star6E Sensor Modes
# Discovers all sensor modes and sweeps FPS values to detect anomalies
# (e.g., requested 120 but got 70).

HOST="${HOST:-root@192.168.1.13}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/waybeam_venc_test}"
SENSOR_INDEX=0
FPS_LIST="25,30,50,60,90,120,144"
TIMEOUT_PER_RUN=15
SKIP_BUILD=0
CODEC="265cbr"
REBOOT_BETWEEN=0
SSH_CONNECT_TIMEOUT="${SSH_CONNECT_TIMEOUT:-3}"
SSH_PROBE_TIMEOUT="${SSH_PROBE_TIMEOUT:-5}"
STREAM_HOST="192.168.1.2"
ISP_BIN="/etc/sensors/imx335_greg_fpvVII-gpt200.bin"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)          HOST="$2";            shift 2 ;;
    --sensor-index)  SENSOR_INDEX="$2";    shift 2 ;;
    --fps-list)      FPS_LIST="$2";        shift 2 ;;
    --timeout-per-run) TIMEOUT_PER_RUN="$2"; shift 2 ;;
    --skip-build)    SKIP_BUILD=1;         shift ;;
    --codec)         CODEC="$2";           shift 2 ;;
    --reboot-between) REBOOT_BETWEEN=1;    shift ;;
    --stream-host)   STREAM_HOST="$2";     shift 2 ;;
    --isp-bin)       ISP_BIN="$2";         shift 2 ;;
    --no-isp-bin)    ISP_BIN="";           shift ;;
    --help|-h)
      cat <<'USAGE'
Usage: fps_sweep_test.sh [OPTIONS]

Options:
  --host HOST             SSH target (default: root@192.168.1.13)
  --sensor-index N        Sensor index (default: 0)
  --fps-list "F1,F2,..."  Comma-separated FPS values (default: 25,30,50,60,90,120,144)
  --timeout-per-run SECS  Timeout per venc run (default: 15)
  --skip-build            Reuse already-deployed binary
  --codec CODEC           Codec string (default: 265cbr)
  --reboot-between        Reboot device between sensor mode changes
  --stream-host IP        Stream destination IP (default: 192.168.1.2)
  --isp-bin PATH          ISP bin on device (default: /etc/sensors/imx335_greg_fpvVII-gpt200.bin)
  --no-isp-bin            Run without ISP bin
  --help                  Show this help
USAGE
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE_TEST="${ROOT_DIR}/scripts/remote_test.sh"

IFS=',' read -ra FPS_VALUES <<< "${FPS_LIST}"

# --- SSH ControlMaster multiplexing ---
SSH_CONTROL_DIR="$(mktemp -d "${TMPDIR:-/tmp}/fps_sweep_ssh.XXXXXX")"
SSH_CONTROL_PATH="${SSH_CONTROL_DIR}/ctrl-%C"
SSH_MUX_OPTS=(-o "ControlPath=${SSH_CONTROL_PATH}")

cleanup_ssh_mux() {
  ssh "${SSH_MUX_OPTS[@]}" -O exit "${HOST}" 2>/dev/null || true
  rm -rf "${SSH_CONTROL_DIR}" 2>/dev/null || true
}
trap cleanup_ssh_mux EXIT

start_ssh_mux() {
  ssh -o "ControlMaster=yes" -o "ControlPersist=120" \
    "${SSH_MUX_OPTS[@]}" \
    -o "BatchMode=yes" -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" \
    -fnN "${HOST}" 2>/dev/null || true
}

# --- Helpers ---

wait_for_ssh() {
  local attempts="${1:-20}"
  local delay="${2:-1}"
  local i
  for ((i=1; i<=attempts; i++)); do
    if timeout "${SSH_PROBE_TIMEOUT}" \
      ssh "${SSH_MUX_OPTS[@]}" -o BatchMode=yes -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" \
      "echo up" >/dev/null 2>&1
    then
      return 0
    fi
    sleep "${delay}"
  done
  return 1
}

remote_ssh() {
  ssh "${SSH_MUX_OPTS[@]}" -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" "$@"
}

remote_run_venc() {
  # Run venc remotely with given args, capture stdout+stderr, subject to timeout
  local args="$1"
  local tout="${2:-${TIMEOUT_PER_RUN}}"
  timeout "${tout}" ssh "${SSH_MUX_OPTS[@]}" -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" \
    "cd ${REMOTE_DIR} && export LD_LIBRARY_PATH=/usr/lib && ${REMOTE_DIR}/venc ${args}" 2>&1 || true
}

reboot_device() {
  echo "[fps_sweep] Rebooting device..."
  set +e
  timeout "${SSH_PROBE_TIMEOUT}" \
    ssh "${SSH_MUX_OPTS[@]}" -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" "reboot" >/dev/null 2>&1
  set -e
  sleep 3
  if ! wait_for_ssh 40 2; then
    echo "[fps_sweep] ERROR: Device did not return after reboot."
    exit 2
  fi
  echo "[fps_sweep] Device is back."
  # Re-establish mux after reboot (old master socket is dead).
  start_ssh_mux
  # Stop majestic after reboot
  remote_ssh '
    if command -v killall >/dev/null 2>&1; then killall majestic 2>/dev/null || true; fi
  ' || true
  sleep 2
}

kill_remote_venc() {
  remote_ssh "
    if command -v killall >/dev/null 2>&1; then killall venc 2>/dev/null || true; fi
    ps | grep '${REMOTE_DIR}/venc' | grep -v grep | while read pid rest; do kill -9 \"\$pid\" 2>/dev/null || true; done
  " 2>/dev/null || true
}

# --- Step 1: Build and deploy via remote_test.sh ---

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  echo "[fps_sweep] Building and deploying via remote_test.sh..."
  "${REMOTE_TEST}" --host "${HOST}" --soc-build star6e --timeout-sec 5 \
    --run-bin venc -- --help >/dev/null 2>&1 || true
  echo "[fps_sweep] Build and deploy complete."
else
  echo "[fps_sweep] Skipping build (--skip-build). Assuming binary is deployed."
fi

# Verify connectivity
if [[ -n "${ISP_BIN}" ]]; then
  echo "[fps_sweep] ISP bin: ${ISP_BIN}"
else
  echo "[fps_sweep] ISP bin: none (WARNING: FPS may be clamped without ISP bin)"
fi
echo "[fps_sweep] Verifying SSH to ${HOST}..."
if ! wait_for_ssh 5 1; then
  echo "[fps_sweep] ERROR: Cannot reach ${HOST}" >&2
  exit 2
fi
start_ssh_mux

# --- Step 2: Discover sensor modes ---

echo "[fps_sweep] Discovering sensor modes (sensor-index ${SENSOR_INDEX})..."
MODE_OUTPUT="$(remote_run_venc "--list-sensor-modes --sensor-index ${SENSOR_INDEX}" 15)"

# Parse mode lines:  "  - [0] 2592x1944 min/max fps 15/30 desc "...""
declare -a MODE_INDICES=()
declare -a MODE_WIDTHS=()
declare -a MODE_HEIGHTS=()
declare -a MODE_MIN_FPS=()
declare -a MODE_MAX_FPS=()

while IFS= read -r line; do
  # Match: "  - [N] WxH min/max fps MIN/MAX"
  if [[ "${line}" =~ \[([0-9]+)\][[:space:]]+([0-9]+)x([0-9]+)[[:space:]]+min/max[[:space:]]+fps[[:space:]]+([0-9]+)/([0-9]+) ]]; then
    MODE_INDICES+=("${BASH_REMATCH[1]}")
    MODE_WIDTHS+=("${BASH_REMATCH[2]}")
    MODE_HEIGHTS+=("${BASH_REMATCH[3]}")
    MODE_MIN_FPS+=("${BASH_REMATCH[4]}")
    MODE_MAX_FPS+=("${BASH_REMATCH[5]}")
  fi
done <<< "${MODE_OUTPUT}"

NUM_MODES=${#MODE_INDICES[@]}
if [[ "${NUM_MODES}" -eq 0 ]]; then
  echo "[fps_sweep] ERROR: No sensor modes discovered. Raw output:"
  echo "${MODE_OUTPUT}"
  exit 1
fi

echo "[fps_sweep] Found ${NUM_MODES} sensor mode(s):"
for ((i=0; i<NUM_MODES; i++)); do
  echo "  [${MODE_INDICES[$i]}] ${MODE_WIDTHS[$i]}x${MODE_HEIGHTS[$i]}  fps ${MODE_MIN_FPS[$i]}-${MODE_MAX_FPS[$i]}"
done

# --- Step 3: FPS sweep ---

# Results arrays
declare -a RES_MODE=()
declare -a RES_RESOLUTION=()
declare -a RES_RANGE=()
declare -a RES_REQUESTED=()
declare -a RES_SENSOR_FPS=()
declare -a RES_AE_FPS=()
declare -a RES_STATUS=()

LAST_MODE=""

for ((m=0; m<NUM_MODES; m++)); do
  mi="${MODE_INDICES[$m]}"
  mw="${MODE_WIDTHS[$m]}"
  mh="${MODE_HEIGHTS[$m]}"
  min_fps="${MODE_MIN_FPS[$m]}"
  max_fps="${MODE_MAX_FPS[$m]}"

  echo ""
  echo "[fps_sweep] === Mode ${mi}: ${mw}x${mh} (${min_fps}-${max_fps} fps) ==="

  # Reboot between mode changes if requested
  if [[ "${REBOOT_BETWEEN}" -eq 1 && "${LAST_MODE}" != "" && "${LAST_MODE}" != "${mi}" ]]; then
    reboot_device
  fi
  LAST_MODE="${mi}"

  for fps in "${FPS_VALUES[@]}"; do
    echo -n "[fps_sweep]   mode=${mi} fps=${fps} ... "

    # Kill any leftover venc and let sensor driver settle
    kill_remote_venc
    sleep 3

    # Run venc
    ISP_BIN_ARG=""
    if [[ -n "${ISP_BIN}" ]]; then
      ISP_BIN_ARG="--isp-bin ${ISP_BIN}"
    fi
    OUTPUT="$(remote_run_venc "--sensor-index ${SENSOR_INDEX} --sensor-mode ${mi} -f ${fps} -h ${STREAM_HOST} -c ${CODEC} ${ISP_BIN_ARG}" "${TIMEOUT_PER_RUN}")"

    # Always kill venc after capture (timeout sends SIGTERM but SSH disconnect may leave it)
    kill_remote_venc

    # Parse sensor-configured FPS from "Sensor: WxH @ FPS" line
    SENSOR_FPS=""
    if [[ "${OUTPUT}" =~ Sensor:[[:space:]]+[0-9]+x[0-9]+[[:space:]]+@[[:space:]]+([0-9]+) ]]; then
      SENSOR_FPS="${BASH_REMATCH[1]}"
    fi

    # Parse actual AE FPS from "[sstar ae_init], FNum = N, fps = N"
    AE_FPS=""
    if [[ "${OUTPUT}" =~ \[sstar\ ae_init\].*fps\ =\ ([0-9]+) ]]; then
      AE_FPS="${BASH_REMATCH[1]}"
    fi

    # The real accepted FPS is what AE reports; sensor line is just configuration
    ACTUAL_FPS="${AE_FPS:-${SENSOR_FPS}}"

    # Determine status based on actual AE FPS vs requested
    STATUS="N/A"
    if [[ -n "${ACTUAL_FPS}" ]]; then
      if [[ "${ACTUAL_FPS}" -eq "${fps}" ]]; then
        STATUS="OK"
      elif [[ "${fps}" -gt "${max_fps}" && "${ACTUAL_FPS}" -eq "${max_fps}" ]]; then
        STATUS="CLAMPED"
      elif [[ "${fps}" -lt "${min_fps}" && "${ACTUAL_FPS}" -eq "${min_fps}" ]]; then
        STATUS="CLAMPED"
      else
        STATUS="ANOMALY"
      fi
    else
      if echo "${OUTPUT}" | grep -qi "fail\|abort\|panic\|segfault\|Unknown argument"; then
        STATUS="ERROR"
      else
        STATUS="NO_DATA"
      fi
    fi

    echo "sensor=${SENSOR_FPS:-?} ae_fps=${AE_FPS:-?}  ${STATUS}"

    # Store result
    RES_MODE+=("${mi}")
    RES_RESOLUTION+=("${mw}x${mh}")
    RES_RANGE+=("${min_fps}-${max_fps}")
    RES_REQUESTED+=("${fps}")
    RES_SENSOR_FPS+=("${SENSOR_FPS:-?}")
    RES_AE_FPS+=("${AE_FPS:-?}")
    RES_STATUS+=("${STATUS}")

    # Liveness check — abort early if device hung
    if ! wait_for_ssh 3 2; then
      echo "[fps_sweep] WARNING: Device unresponsive after mode=${mi} fps=${fps}. Aborting sweep."
      echo "[fps_sweep] Device may need a power cycle."
      break 2
    fi

    # Settle time for sensor driver between runs
    sleep 4
  done
done

# Kill any remaining venc
kill_remote_venc

# --- Step 4: Summary table ---

echo ""
echo "=== FPS Sweep Results ==="
printf "%-6s %-14s %-16s %-10s %-12s %-10s %-10s\n" \
  "Mode" "Resolution" "Mode FPS Range" "Requested" "Sensor Cfg" "AE Actual" "Status"
printf "%-6s %-14s %-16s %-10s %-12s %-10s %-10s\n" \
  "----" "----------" "--------------" "---------" "----------" "---------" "------"

ANOMALY_COUNT=0
ERROR_COUNT=0
for ((i=0; i<${#RES_MODE[@]}; i++)); do
  printf "%-6s %-14s %-16s %-10s %-12s %-10s %-10s\n" \
    "${RES_MODE[$i]}" "${RES_RESOLUTION[$i]}" "${RES_RANGE[$i]}" \
    "${RES_REQUESTED[$i]}" "${RES_SENSOR_FPS[$i]}" "${RES_AE_FPS[$i]}" "${RES_STATUS[$i]}"
  if [[ "${RES_STATUS[$i]}" == "ANOMALY" ]]; then
    ((ANOMALY_COUNT++)) || true
  fi
  if [[ "${RES_STATUS[$i]}" == "ERROR" || "${RES_STATUS[$i]}" == "NO_DATA" ]]; then
    ((ERROR_COUNT++)) || true
  fi
done

echo ""
echo "Total runs: ${#RES_MODE[@]}"
echo "Anomalies:  ${ANOMALY_COUNT}"
echo "Errors:     ${ERROR_COUNT}"

if [[ "${ANOMALY_COUNT}" -gt 0 ]]; then
  echo ""
  echo "!!! ANOMALIES DETECTED !!!"
  echo "Sensor was configured correctly but AE reports different actual FPS:"
  for ((i=0; i<${#RES_MODE[@]}; i++)); do
    if [[ "${RES_STATUS[$i]}" == "ANOMALY" ]]; then
      echo "  Mode ${RES_MODE[$i]} (${RES_RESOLUTION[$i]}): requested=${RES_REQUESTED[$i]} sensor_cfg=${RES_SENSOR_FPS[$i]} ae_actual=${RES_AE_FPS[$i]} (range ${RES_RANGE[$i]})"
    fi
  done
fi

exit 0
