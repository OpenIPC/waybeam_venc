#!/usr/bin/env bash
set -euo pipefail

# Pull verified runtime artifacts from a working Maruko (Infinity6C) device
# into the repo cache. Run once per bring-up or after a firmware/SDK refresh.
#
# Populates:
#   vendor-libs/maruko/            ← MI vendor .so files (with MD5SUMS)
#   sensors/maruko/                ← sensor .ko kernel modules
#   iq-profiles/maruko-bin/        ← ISP tuning .bin blobs from /etc/sensors/
#   vendor-libs/maruko/device-info.txt  ← kernel/firmware provenance stamp
#
# After running, diff-check the result, commit what changed, and the deploy
# script (scripts/maruko_direct_deploy.sh) will push it back to other devices.

HOST="${HOST:-root@192.168.2.12}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LIBS_DST="${ROOT_DIR}/vendor-libs/maruko"
KO_DST="${ROOT_DIR}/sensors/maruko"
BIN_DST="${ROOT_DIR}/iq-profiles/maruko-bin"

REMOTE_LIB_DIR="/usr/lib"
REMOTE_KO_DIR="/lib/modules/5.10.61/sigmastar"
REMOTE_BIN_DIR="/etc/sensors"

# MI libs we expect to find on a working Maruko device. libmi_rgn and
# libmi_vpe are optional (debug OSD / Star6E-only) — pulled if present.
EXPECTED_LIBS=(
	libmi_sys.so
	libmi_isp.so
	libmi_venc.so
	libmi_scl.so
	libmi_sensor.so
	libmi_vif.so
	libmi_common.so
	libmi_ai.so
	libmi_ao.so
	libispalgo.so
	libcam_os_wrapper.so
	libcus3a.so
)
OPTIONAL_LIBS=(
	libmi_rgn.so
	libopus.so
	libopus.so.0
	libopus.so.0.8.0
)

usage() {
	cat <<EOF
Usage: scripts/maruko_pull_artifacts.sh [--host HOST] [--dry-run] [components...]

Pull verified Maruko runtime artifacts from a working device.

Default host:  ${HOST}
Components:    libs, drivers, isp-bins, info  (default: all)

Options:
  --host HOST     SSH target (default: root@192.168.2.12)
  --dry-run       List what would be copied without writing
  --help          Show this help

Examples:
  scripts/maruko_pull_artifacts.sh
  scripts/maruko_pull_artifacts.sh --host root@192.168.2.13
  scripts/maruko_pull_artifacts.sh libs isp-bins
EOF
}

log() { printf '[maruko_pull] %s\n' "$*"; }
warn() { printf '[maruko_pull] WARNING: %s\n' "$*" >&2; }
die() { printf '[maruko_pull] ERROR: %s\n' "$*" >&2; exit 1; }

DRY_RUN=0
COMPONENTS=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--host)    HOST="$2"; shift 2 ;;
		--dry-run) DRY_RUN=1; shift ;;
		--help|-h) usage; exit 0 ;;
		libs|drivers|isp-bins|info) COMPONENTS+=("$1"); shift ;;
		*) die "unknown argument: $1" ;;
	esac
done
if [[ ${#COMPONENTS[@]} -eq 0 ]]; then
	COMPONENTS=(libs drivers isp-bins info)
fi

remote_sh()  { ssh -n -o BatchMode=yes -o ConnectTimeout=10 "${HOST}" "$@"; }
remote_cat() { ssh -n -o BatchMode=yes -o ConnectTimeout=10 "${HOST}" "cat $1"; }

probe_remote() {
	log "Probing ${HOST} ..."
	remote_sh "test -d ${REMOTE_LIB_DIR}" || die "missing ${REMOTE_LIB_DIR} on remote"
}

pull_file() {
	# pull_file REMOTE_PATH LOCAL_PATH
	local src="$1" dst="$2"
	if [[ "${DRY_RUN}" -eq 1 ]]; then
		printf '  DRY  %s -> %s\n' "${src}" "${dst}"
		return 0
	fi
	mkdir -p "$(dirname "${dst}")"
	if remote_cat "${src}" > "${dst}.partial" 2>/dev/null; then
		mv "${dst}.partial" "${dst}"
		printf '  OK   %s (%s bytes)\n' "${dst#${ROOT_DIR}/}" "$(stat -c%s "${dst}")"
		return 0
	else
		rm -f "${dst}.partial"
		return 1
	fi
}

pull_libs() {
	log "Pulling MI vendor libs from ${HOST}:${REMOTE_LIB_DIR}"
	local lib
	for lib in "${EXPECTED_LIBS[@]}"; do
		if ! pull_file "${REMOTE_LIB_DIR}/${lib}" "${LIBS_DST}/${lib}"; then
			warn "expected lib not found on device: ${lib}"
		fi
	done
	for lib in "${OPTIONAL_LIBS[@]}"; do
		pull_file "${REMOTE_LIB_DIR}/${lib}" "${LIBS_DST}/${lib}" || true
	done
	if [[ "${DRY_RUN}" -eq 0 ]]; then
		log "Regenerating MD5SUMS"
		( cd "${LIBS_DST}" && md5sum *.so 2>/dev/null | sort > MD5SUMS )
	fi
}

pull_drivers() {
	log "Pulling sensor .ko from ${HOST}:${REMOTE_KO_DIR}"
	local ko
	# Pull only the sensor_* and drv_* modules; ignore other vendor kos.
	local listing
	listing="$(remote_sh "ls ${REMOTE_KO_DIR}/sensor_*.ko ${REMOTE_KO_DIR}/drv_ms_cus_*.ko 2>/dev/null" || true)"
	if [[ -z "${listing}" ]]; then
		warn "no sensor .ko found under ${REMOTE_KO_DIR}"
		return 0
	fi
	while IFS= read -r ko; do
		[[ -z "${ko}" ]] && continue
		pull_file "${ko}" "${KO_DST}/$(basename "${ko}")" || warn "failed to pull ${ko}"
	done <<< "${listing}"
}

pull_isp_bins() {
	log "Pulling ISP tuning .bin from ${HOST}:${REMOTE_BIN_DIR}"
	local listing
	listing="$(remote_sh "ls ${REMOTE_BIN_DIR}/*.bin 2>/dev/null" || true)"
	if [[ -z "${listing}" ]]; then
		warn "no .bin found under ${REMOTE_BIN_DIR} (device may use a non-standard path)"
		return 0
	fi
	local f
	while IFS= read -r f; do
		[[ -z "${f}" ]] && continue
		pull_file "${f}" "${BIN_DST}/$(basename "${f}")" || warn "failed to pull ${f}"
	done <<< "${listing}"
}

pull_info() {
	log "Recording device provenance"
	local info_path="${LIBS_DST}/device-info.txt"
	if [[ "${DRY_RUN}" -eq 1 ]]; then
		printf '  DRY  %s\n' "${info_path#${ROOT_DIR}/}"
		return 0
	fi
	mkdir -p "$(dirname "${info_path}")"
	{
		echo "# Maruko device snapshot"
		echo "# Pulled: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "# Host:   ${HOST}"
		echo
		echo "## uname -a"
		remote_sh "uname -a" 2>/dev/null || echo "(unavailable)"
		echo
		echo "## /etc/os-release"
		remote_sh "cat /etc/os-release 2>/dev/null" || echo "(unavailable)"
		echo
		echo "## /proc/cpuinfo (head)"
		remote_sh "head -20 /proc/cpuinfo 2>/dev/null" || echo "(unavailable)"
		echo
		echo "## kernel module versions"
		remote_sh "find ${REMOTE_KO_DIR} -maxdepth 1 -name '*.ko' -printf '%f %s\n' 2>/dev/null" || true
		echo
		echo "## /etc/venc.json (if present)"
		remote_sh "cat /etc/venc.json 2>/dev/null" || echo "(not present)"
	} > "${info_path}"
	printf '  OK   %s\n' "${info_path#${ROOT_DIR}/}"
}

probe_remote
for c in "${COMPONENTS[@]}"; do
	case "${c}" in
		libs)     pull_libs ;;
		drivers)  pull_drivers ;;
		isp-bins) pull_isp_bins ;;
		info)     pull_info ;;
	esac
done

log "Done. Review with: git status && git diff --stat"
