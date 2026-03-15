#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_SCRIPT="${SCRIPT_DIR}/remote_test.sh"

UDP_HOST="${UDP_HOST:-}"
UDP_PORT="${UDP_PORT:-}"
UDP_TAG="${UDP_TAG:-star6e-test}"

PASS_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --udp-host)
      UDP_HOST="$2"
      shift 2
      ;;
    --udp-port)
      UDP_PORT="$2"
      shift 2
      ;;
    --udp-tag)
      UDP_TAG="$2"
      shift 2
      ;;
    --)
      shift
      PASS_ARGS+=("$@")
      break
      ;;
    *)
      PASS_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ -z "${UDP_HOST}" || -z "${UDP_PORT}" ]]; then
  echo "Usage: $0 --udp-host <host> --udp-port <port> [--udp-tag <tag>] [remote_test args...]"
  echo "Example:"
  echo "  $0 --udp-host 192.168.1.20 --udp-port 19000 -- --run-bin snr_sequence_probe -- --majestic-order"
  exit 1
fi

if [[ ! -x "${BASE_SCRIPT}" ]]; then
  echo "ERROR: base script not executable: ${BASE_SCRIPT}"
  exit 1
fi

seqno=0
set +e
"${BASE_SCRIPT}" "${PASS_ARGS[@]}" 2>&1 | while IFS= read -r line; do
  seqno=$((seqno + 1))
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  msg="[${UDP_TAG}] [${ts}] [${seqno}] ${line}"
  printf '%s\n' "${msg}"
  # UDP write failures must not stop the local test runner.
  printf '%s\n' "${msg}" >"/dev/udp/${UDP_HOST}/${UDP_PORT}" 2>/dev/null || true
done
ret=${PIPESTATUS[0]}
set -e

exit "${ret}"
