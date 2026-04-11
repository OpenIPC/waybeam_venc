#!/usr/bin/env bash
set -euo pipefail

# Bounded remote runner for CLI-driven probes and short-lived test binaries.
# For long-running Star6E venc validation against the production
# /etc/venc.json workflow, prefer scripts/star6e_direct_deploy.sh.

HOST="${HOST:-root@192.168.1.11}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/waybeam_venc_test}"
TIMEOUT_SEC="${TIMEOUT_SEC:-10}"
RUN_BIN="snr_toggle_test"
SOC_BUILD="${SOC_BUILD:-auto}"
MARUKO_ISP_BIN="${MARUKO_ISP_BIN:-/etc/sensors/imx415.bin}"
PREWARM_MAJESTIC=0
PREWARM_SECONDS=7
CHECK_DMESG=1
DMESG_KEYWORDS="${DMESG_KEYWORDS:-snr|vif|isp|venc|mi_|fault|oops|panic|watchdog|segmentation|timeout}"
REBOOT_BEFORE_RUN=0
REBOOT_WAIT_SEC=12
IOCTL_TRACE=0
SSH_CONNECT_TIMEOUT="${SSH_CONNECT_TIMEOUT:-3}"
SSH_PROBE_TIMEOUT="${SSH_PROBE_TIMEOUT:-5}"
JSON_SUMMARY=0
SKIP_DEPLOY=0
SKIP_BUILD=0

usage() {
	cat <<'EOF'
Usage: scripts/remote_test.sh [options] [-- run-bin-args...]

Bounded remote runner for CLI-driven probes and short-lived test binaries.
For long-running Star6E venc validation against the production /etc/venc.json
path, prefer:

  scripts/star6e_direct_deploy.sh cycle

Options:
  --host HOST              SSH target
  --soc-build BUILD        auto | star6e | maruko
  --remote-dir DIR         Remote staging dir (default: /tmp/waybeam_venc_test)
  --timeout-sec SEC        Remote run timeout (default: 10)
  --run-bin BIN            venc | snr_toggle_test | snr_sequence_probe
  --prewarm-majestic       Start majestic briefly before the test
  --prewarm-seconds SEC    Prewarm duration (default: 7)
  --no-dmesg               Skip dmesg delta collection
  --reboot-before-run      Reboot target before running
  --reboot-wait-sec SEC    Reconnect wait after reboot (default: 12)
  --ioctl-trace            Preload ioctl trace shim
  --json-summary           Emit machine-readable JSON summary at the end
  --skip-build             Reuse current local build
  --skip-deploy            Reuse already deployed binary/runtime bundle
  --help                   Show this help

Examples:
  scripts/remote_test.sh --host root@192.168.1.13 --soc-build star6e --run-bin venc -- --list-sensor-modes --sensor-index 0
  scripts/remote_test.sh --host root@192.168.2.12 --soc-build maruko --run-bin snr_toggle_test -- --sensor-index 0 --sensor-mode 1 -f 30
EOF
}

RUN_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --soc-build)
      SOC_BUILD="$2"
      shift 2
      ;;
    --host)
      HOST="$2"
      shift 2
      ;;
    --remote-dir)
      REMOTE_DIR="$2"
      shift 2
      ;;
    --timeout-sec)
      TIMEOUT_SEC="$2"
      shift 2
      ;;
    --run-bin)
      RUN_BIN="$2"
      shift 2
      ;;
    --prewarm-majestic)
      PREWARM_MAJESTIC=1
      shift
      ;;
    --prewarm-seconds)
      PREWARM_SECONDS="$2"
      shift 2
      ;;
    --no-dmesg)
      CHECK_DMESG=0
      shift
      ;;
    --reboot-before-run)
      REBOOT_BEFORE_RUN=1
      shift
      ;;
    --reboot-wait-sec)
      REBOOT_WAIT_SEC="$2"
      shift 2
      ;;
    --ioctl-trace)
      IOCTL_TRACE=1
      shift
      ;;
    --json-summary)
      JSON_SUMMARY=1
      shift
      ;;
    --skip-deploy)
      SKIP_DEPLOY=1
      shift
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --)
      shift
      RUN_ARGS+=("$@")
      break
      ;;
    *)
      RUN_ARGS+=("$1")
      shift
      ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAR6E_CC="${ROOT_DIR}/toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-gcc"
SOC_BUILD_RESOLVED=""
REMOTE_FAMILY=""
MARUKO_RUNTIME_DIR="${MARUKO_RUNTIME_DIR:-${ROOT_DIR}/libs/maruko}"
MARUKO_RUNTIME_LIBS=(
  libcam_os_wrapper.so
  libcus3a.so
  libispalgo.so
  libmi_common.so
  libmi_isp.so
  libmi_scl.so
  libmi_sensor.so
  libmi_sys.so
  libmi_venc.so
  libmi_vif.so
  libmi_vpe.so
)

# --- SSH ControlMaster multiplexing ---
# Opens one persistent TCP connection; all subsequent ssh calls reuse it.
SSH_CONTROL_DIR="$(mktemp -d "${TMPDIR:-/tmp}/remote_test_ssh.XXXXXX")"
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

# Wrapper: use multiplexed connection for all SSH calls.
remote_ssh() {
  ssh "${SSH_MUX_OPTS[@]}" -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${HOST}" "$@"
}

# Wrapper: copy file via ssh stream (avoids scp/sftp dependency).
copy_file_ssh() {
  local src="$1"
  local dst="$2"
  ssh "${SSH_MUX_OPTS[@]}" -o "ConnectTimeout=${SSH_CONNECT_TIMEOUT}" "${HOST}" "cat > '${dst}'" < "${src}"
}

require_local_file() {
  local path="$1"
  if [[ ! -e "${path}" ]]; then
    echo "[remote_test] ERROR: required file not found: ${path}"
    exit 1
  fi
}

build_ioctl_trace_preload_if_needed() {
  local src="${ROOT_DIR}/tools/ioctl_trace_preload.c"
  local out="${ROOT_DIR}/tools/ioctl_trace_preload.so"
  if [[ -f "${out}" ]]; then
    return 0
  fi
  if [[ ! -x "${STAR6E_CC}" ]]; then
    echo "[remote_test] ERROR: --ioctl-trace requested, but compiler is missing: ${STAR6E_CC}"
    return 1
  fi
  if [[ ! -f "${src}" ]]; then
    echo "[remote_test] ERROR: --ioctl-trace requested, missing source: ${src}"
    return 1
  fi
  echo "[remote_test] Building ioctl trace preload helper..."
  "${STAR6E_CC}" -shared -fPIC -O2 "${src}" -ldl -o "${out}"
}

wait_for_ssh() {
  local attempts="${1:-20}"
  local delay="${2:-1}"
  local i
  for ((i=1; i<=attempts; i++)); do
    if timeout "${SSH_PROBE_TIMEOUT}" \
      ssh "${SSH_MUX_OPTS[@]}" -o BatchMode=yes -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" \
      "echo up:\$(hostname)" >/dev/null 2>&1
    then
      return 0
    fi
    sleep "${delay}"
  done
  return 1
}

wait_for_ssh_down() {
  local attempts="${1:-20}"
  local delay="${2:-1}"
  local i
  for ((i=1; i<=attempts; i++)); do
    if ! timeout "${SSH_PROBE_TIMEOUT}" \
      ssh "${SSH_MUX_OPTS[@]}" -o BatchMode=yes -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" \
      "echo up:\$(hostname)" >/dev/null 2>&1
    then
      return 0
    fi
    sleep "${delay}"
  done
  return 1
}

stop_majestic_and_detect_family() {
  echo "[remote_test] Stopping majestic and detecting family..."
  local result
  set +e
  result="$(remote_ssh '
    killall -q majestic 2>/dev/null; killall -q -9 majestic 2>/dev/null; true
    echo "---FAMILY---"
    ipcinfo --family 2>/dev/null || cat /proc/device-tree/compatible 2>/dev/null | tr "\0" " " || echo "unknown"
  ')"
  local rc=$?
  set -e
  if [[ ${rc} -ne 0 ]]; then
    echo "[remote_test] WARNING: majestic stop returned ${rc}; waiting for SSH..."
    if wait_for_ssh 5 1; then
      echo "[remote_test] SSH recovered."
      result="$(remote_ssh 'ipcinfo --family 2>/dev/null || echo unknown')"
    else
      echo "[remote_test] WARNING: SSH did not recover after majestic stop."
    fi
  fi
  local family
  family="$(echo "${result}" | sed -n '/---FAMILY---/,$ p' | tail -n +2 | tr '[:upper:]' '[:lower:]' | tr -d '\r' | tr '\n' ' ')"
  REMOTE_FAMILY="$(echo "${family}" | sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//')"
}

resolve_soc_build() {
  case "${SOC_BUILD}" in
    star6e|maruko)
      SOC_BUILD_RESOLVED="${SOC_BUILD}"
      ;;
    auto)
      if [[ "${REMOTE_FAMILY}" == *"infinity6c"* || "${REMOTE_FAMILY}" == *"maruko"* || "${REMOTE_FAMILY}" == *"ssc377"* ]]; then
        SOC_BUILD_RESOLVED="maruko"
      else
        SOC_BUILD_RESOLVED="star6e"
      fi
      ;;
    *)
      echo "[remote_test] ERROR: --soc-build must be one of: auto, star6e, maruko"
      exit 1
      ;;
  esac
}

echo "[remote_test] Checking SSH connectivity to ${HOST}..."
timeout "${SSH_PROBE_TIMEOUT}" \
  ssh "${SSH_MUX_OPTS[@]}" -o BatchMode=yes -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" \
  "echo connected:\$(hostname)"

# Start persistent mux connection after first successful probe.
start_ssh_mux

stop_majestic_and_detect_family
resolve_soc_build
echo "[remote_test] Remote family hint: ${REMOTE_FAMILY:-unknown}; build target: ${SOC_BUILD_RESOLVED}"

if [[ "${SOC_BUILD_RESOLVED}" == "maruko" && "${RUN_BIN}" == "snr_toggle_test" ]]; then
  RUN_BIN="venc"
  echo "[remote_test] Default run-bin switched to venc for maruko build."
fi

if [[ "${SOC_BUILD_RESOLVED}" == "maruko" ]]; then
  HAS_ISP_BIN=0
  for ((i=0; i<${#RUN_ARGS[@]}; i++)); do
    if [[ "${RUN_ARGS[$i]}" == "--isp-bin" ]]; then
      HAS_ISP_BIN=1
      break
    fi
  done
  if [[ "${HAS_ISP_BIN}" -eq 0 ]]; then
    RUN_ARGS+=(--isp-bin "${MARUKO_ISP_BIN}")
    echo "[remote_test] Added default Maruko ISP bin: ${MARUKO_ISP_BIN}"
  fi
fi

if [[ "${SKIP_BUILD}" -eq 1 ]]; then
  echo "[remote_test] Skipping build (--skip-build)."
else
  echo "[remote_test] Building standalone binaries (SOC_BUILD=${SOC_BUILD_RESOLVED})..."
  if [[ "${SOC_BUILD_RESOLVED}" == "maruko" ]]; then
    make -C "${ROOT_DIR}" SOC_BUILD=maruko clean >/dev/null
    make -C "${ROOT_DIR}" SOC_BUILD=maruko all
  else
    make -C "${ROOT_DIR}" SOC_BUILD=star6e clean >/dev/null
    make -C "${ROOT_DIR}" SOC_BUILD=star6e all
  fi
fi

if [[ "${REBOOT_BEFORE_RUN}" -eq 1 ]]; then
  echo "[remote_test] Rebooting device to enforce cold state..."
  set +e
  timeout "${SSH_PROBE_TIMEOUT}" \
    ssh "${SSH_MUX_OPTS[@]}" -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" "reboot" >/dev/null 2>&1
  set -e
  if wait_for_ssh_down 12 1; then
    echo "[remote_test] Device went down for reboot."
  else
    echo "[remote_test] WARNING: Did not observe SSH drop before reconnect wait."
  fi
  sleep "${REBOOT_WAIT_SEC}"
  if ! wait_for_ssh 30 1; then
    echo "[remote_test] Device did not return after reboot window."
    echo "[remote_test] Please check power and connectivity, then rerun."
    exit 2
  fi
  echo "[remote_test] Device is back after reboot."
  remote_ssh 'killall -q majestic 2>/dev/null; killall -q -9 majestic 2>/dev/null; true'
fi

echo "[remote_test] Preparing remote environment..."
DMESG_START_LINES=0
PRERUN_RESULT="$(remote_ssh "
  mkdir -p '${REMOTE_DIR}'
  for p in venc snr_toggle_test snr_sequence_probe; do
    pid=\$(pidof \"\${p}\" 2>/dev/null) && kill -9 \${pid} 2>/dev/null || true
  done
  echo \$(dmesg | wc -l)
")"
if [[ "${CHECK_DMESG}" -eq 1 ]]; then
  DMESG_START_LINES="$(echo "${PRERUN_RESULT}" | tail -1 | tr -d '[:space:]')"
  DMESG_START_LINES="${DMESG_START_LINES:-0}"
  echo "[remote_test] dmesg baseline: ${DMESG_START_LINES} lines"
fi

deploy_binaries() {
  local bin="${ROOT_DIR}/out/${SOC_BUILD_RESOLVED}/${RUN_BIN}"
  local lib
  if [[ ! -f "${bin}" ]]; then
    echo "[remote_test] ERROR: binary not found: ${bin}"
    exit 1
  fi
  echo "[remote_test] Deploying ${RUN_BIN}..."
  copy_file_ssh "${bin}" "${REMOTE_DIR}/${RUN_BIN}"
  if [[ "${IOCTL_TRACE}" -eq 1 ]]; then
    build_ioctl_trace_preload_if_needed
    remote_ssh "mkdir -p '${REMOTE_DIR}/lib'"
    copy_file_ssh "${ROOT_DIR}/tools/ioctl_trace_preload.so" \
      "${REMOTE_DIR}/lib/ioctl_trace_preload.so"
  fi
  remote_ssh "chmod +x '${REMOTE_DIR}/${RUN_BIN}'"

  if [[ "${SOC_BUILD_RESOLVED}" == "maruko" ]]; then
    echo "[remote_test] Deploying Maruko runtime bundle..."
    remote_ssh "mkdir -p '${REMOTE_DIR}/lib'"
    for lib in "${MARUKO_RUNTIME_LIBS[@]}"; do
      require_local_file "${MARUKO_RUNTIME_DIR}/${lib}"
      copy_file_ssh "${MARUKO_RUNTIME_DIR}/${lib}" "${REMOTE_DIR}/lib/${lib}"
    done
    # uClibc runtime and shim no longer needed — Maruko uses dlopen for
    # all MI libs, eliminating the musl/uClibc ABI mismatch.
  fi
}

if [[ "${SKIP_DEPLOY}" -eq 1 ]]; then
  echo "[remote_test] Skipping deployment (--skip-deploy)."
else
  deploy_binaries
fi

if [[ "${PREWARM_MAJESTIC}" -eq 1 ]]; then
  echo "[remote_test] Prewarming sensor stack with majestic (${PREWARM_SECONDS}s)..."
  remote_ssh \
    "if command -v majestic >/dev/null 2>&1; then timeout '${PREWARM_SECONDS}' majestic >/tmp/majestic-prewarm.log 2>&1 || true; else echo '[remote_test] WARNING: majestic not found'; fi"
  echo "[remote_test] NOTE: cold-state testing after this requires a reboot."
fi

REMOTE_ARG_STR=""
for arg in "${RUN_ARGS[@]}"; do
  REMOTE_ARG_STR+=" $(printf '%q' "${arg}")"
done

REMOTE_PRELOAD_BLOCK=""
if [[ "${IOCTL_TRACE}" -eq 1 ]]; then
  REMOTE_PRELOAD_BLOCK+=$'if [ -n "${LD_PRELOAD:-}" ]; then\n'
  REMOTE_PRELOAD_BLOCK+=$'  export LD_PRELOAD="${LD_PRELOAD} '"$(printf '%q' "${REMOTE_DIR}/lib/ioctl_trace_preload.so")"$'"\n'
  REMOTE_PRELOAD_BLOCK+=$'else\n'
  REMOTE_PRELOAD_BLOCK+=$'  export LD_PRELOAD='"$(printf '%q' "${REMOTE_DIR}/lib/ioctl_trace_preload.so")"$'\n'
  REMOTE_PRELOAD_BLOCK+=$'fi\n'
fi

REMOTE_LD_PATH="/usr/lib"
if [[ "${SOC_BUILD_RESOLVED}" == "maruko" || "${IOCTL_TRACE}" -eq 1 ]]; then
  REMOTE_LD_PATH="${REMOTE_DIR}/lib:/usr/lib"
fi

REMOTE_SCRIPT=$(
  cat <<EOF
cd $(printf '%q' "${REMOTE_DIR}")
export LD_LIBRARY_PATH=$(printf '%q' "${REMOTE_LD_PATH}")
${REMOTE_PRELOAD_BLOCK}exec $(printf '%q' "${REMOTE_DIR}/${RUN_BIN}")${REMOTE_ARG_STR}
EOF
)

echo "[remote_test] Running ${RUN_BIN} with timeout ${TIMEOUT_SEC}s..."
RUN_START_EPOCH="$(date +%s)"
set +e
timeout "${TIMEOUT_SEC}" \
  ssh "${SSH_MUX_OPTS[@]}" -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" "sh -c $(printf '%q' "${REMOTE_SCRIPT}")"
RUN_RET=$?
set -e
RUN_END_EPOCH="$(date +%s)"
RUN_DURATION=$((RUN_END_EPOCH - RUN_START_EPOCH))

# Classify run result.
RUN_STATUS="success"
if [[ ${RUN_RET} -eq 0 ]]; then
  echo "[remote_test] Remote run completed successfully."
elif [[ ${RUN_RET} -eq 124 ]]; then
  RUN_STATUS="timeout"
  echo "[remote_test] WARNING: Timed out after ${TIMEOUT_SEC}s."
else
  RUN_STATUS="failed"
  echo "[remote_test] WARNING: Remote run exited with status ${RUN_RET}."
fi

echo "[remote_test] Post-run liveness check..."
set +e
timeout "${SSH_PROBE_TIMEOUT}" \
  ssh "${SSH_MUX_OPTS[@]}" -o BatchMode=yes -o ConnectTimeout="${SSH_CONNECT_TIMEOUT}" "${HOST}" "echo alive:\$(hostname)"
PING_RET=$?
set -e

DEVICE_ALIVE=true
if [[ ${PING_RET} -ne 0 ]]; then
  DEVICE_ALIVE=false
  RUN_STATUS="device_unresponsive"
  echo "[remote_test] DEVICE UNRESPONSIVE. Please power cycle the board, then rerun."
fi

DMESG_HITS=0
if [[ "${DEVICE_ALIVE}" == "true" && "${CHECK_DMESG}" -eq 1 ]]; then
  START_LINE=$((DMESG_START_LINES + 1))
  echo "[remote_test] Collecting new dmesg lines from ${START_LINE}..."
  set +e
  NEW_DMESG="$(
    remote_ssh "dmesg | sed -n '${START_LINE},\$p'" 2>/dev/null
  )"
  DMESG_RET=$?
  set -e

  if [[ ${DMESG_RET} -ne 0 ]]; then
    echo "[remote_test] WARNING: Failed to collect dmesg delta."
  elif [[ -z "${NEW_DMESG}" ]]; then
    echo "[remote_test] dmesg delta: no new lines."
  else
    echo "[remote_test] dmesg delta (filtered):"
    FILTERED="$(printf '%s\n' "${NEW_DMESG}" | grep -Ei "${DMESG_KEYWORDS}" || true)"
    if [[ -n "${FILTERED}" ]]; then
      DMESG_HITS="$(printf '%s\n' "${FILTERED}" | wc -l | tr -d '[:space:]')"
      printf '%s\n' "${FILTERED}" | tail -n 120
    else
      echo "  (no keyword matches; showing last 40 new lines)"
      printf '%s\n' "${NEW_DMESG}" | tail -n 40
    fi
  fi
fi

if [[ "${DEVICE_ALIVE}" == "true" ]]; then
  echo "[remote_test] Device is still responsive."
fi

# --- JSON summary (machine-readable output for agent consumption) ---
if [[ "${JSON_SUMMARY}" -eq 1 ]]; then
  printf '{"status":"%s","exit_code":%d,"device_alive":%s,"dmesg_hits":%d,"duration_sec":%d,"run_bin":"%s","soc_build":"%s","host":"%s"}\n' \
    "${RUN_STATUS}" "${RUN_RET}" "${DEVICE_ALIVE}" "${DMESG_HITS}" "${RUN_DURATION}" \
    "${RUN_BIN}" "${SOC_BUILD_RESOLVED}" "${HOST}"
fi

# --- Strict exit codes ---
# 0 = success, 1 = run failed, 2 = device unresponsive, 124 = timeout
if [[ "${RUN_STATUS}" == "device_unresponsive" ]]; then
  exit 2
elif [[ "${RUN_STATUS}" == "timeout" ]]; then
  exit 124
elif [[ "${RUN_STATUS}" == "failed" ]]; then
  exit 1
fi
exit 0
