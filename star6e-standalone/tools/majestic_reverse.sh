#!/usr/bin/env bash
set -euo pipefail

HOST="${1:-root@192.168.1.10}"
REMOTE_BIN="${2:-/usr/bin/majestic}"
OUT_DIR="${3:-/tmp/majestic_reverse}"

mkdir -p "${OUT_DIR}"

BIN_PATH="${OUT_DIR}/majestic"
STAMP="$(date +%Y%m%d_%H%M%S)"
REPORT="${OUT_DIR}/report_${STAMP}.txt"

echo "[*] Fetching ${REMOTE_BIN} from ${HOST} -> ${BIN_PATH}"
ssh -o ConnectTimeout=5 "${HOST}" "cat '${REMOTE_BIN}'" > "${BIN_PATH}"
chmod +x "${BIN_PATH}" || true

{
  echo "== basic =="
  date -u
  echo "host=${HOST}"
  echo "remote_bin=${REMOTE_BIN}"
  echo "bin_path=${BIN_PATH}"
  echo

  echo "== file =="
  file "${BIN_PATH}"
  echo

  echo "== readelf -h =="
  readelf -h "${BIN_PATH}" || true
  echo

  echo "== readelf -d =="
  readelf -d "${BIN_PATH}" || true
  echo

  echo "== init hints (strings) =="
  strings -a "${BIN_PATH}" | grep -E \
    'start_sdk|init_sdk|mi_snr_init|mi_snr_get_resolution|MI_SNR_|MI_VIF_|MI_VPE_|MI_VENC_|MI_ISP_' \
    | sort -u || true
  echo

  echo "== probable snr/vif/venc callsite strings =="
  strings -a "${BIN_PATH}" | grep -E \
    '(start_sdk|init_sdk|mi_snr_|MI_SNR_|mi_vif_|MI_VIF_|mi_vpe_|MI_VPE_|mi_venc_|MI_VENC_|mi_isp_|MI_ISP_)' \
    | sort -u || true
} | tee "${REPORT}"

echo
echo "[*] Wrote report: ${REPORT}"
echo "[*] Next: compare report hints with snr_sequence_probe call order."
