#!/usr/bin/env bash
set -euo pipefail

# Direct Maruko (Infinity6C) deploy-and-test helper. Mirrors
# scripts/star6e_direct_deploy.sh but also handles the Maruko-only
# components: MI vendor libs, sensor .ko modules, and ISP .bin blobs.
#
# Sources of truth in this repo:
#   vendor-libs/maruko/*.so       → /usr/lib/
#   sensors/maruko/*.ko           → /lib/modules/5.10.61/sigmastar/
#   iq-profiles/maruko-bin/*.bin  → /etc/sensors/
#
# Populate them once with scripts/maruko_pull_artifacts.sh from a working
# bench, then this script pushes the same set back to any Maruko device.

HOST="${HOST:-root@192.168.2.12}"
LOCAL_BIN="out/maruko/waybeam"
REMOTE_BIN="/usr/bin/waybeam"
REMOTE_LIB_DIR="/usr/lib"
REMOTE_KO_DIR="/lib/modules/5.10.61/sigmastar"
REMOTE_ISP_BIN_DIR="/etc/sensors"
CONFIG_PATH="/etc/waybeam.json"
LOG_PATH="/tmp/waybeam.log"
LATEST_BACKUP_PATH="/tmp/waybeam.json.bak.latest"
INIT_SCRIPT_LOCAL="init.d/S95waybeam"
INIT_SCRIPT_REMOTE="/etc/init.d/S95waybeam"
WAIT_SECS=20
TAIL_LINES=120
HTTP_PORT="${HTTP_PORT:-}"
SKIP_BUILD=0
SKIP_BACKUP=0
WITH_LIBS=0
WITH_DRIVERS=0
WITH_ISP_BINS=0
WITH_JSON_CLI=0
REBOOT_AFTER_DRIVERS=0
BACKUP_PATH=""
PUSH_CONFIG_FILE=""
COMMAND="cycle"
COMMAND_ARGS=()

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBS_DIR="${ROOT_DIR}/vendor-libs/maruko"
KO_DIR="${ROOT_DIR}/sensors/maruko"
ISP_BIN_DIR="${ROOT_DIR}/iq-profiles/maruko-bin"
JSON_CLI_LOCAL="${ROOT_DIR}/out/maruko/json_cli"
JSON_CLI_REMOTE="/usr/bin/json_cli"

usage() {
	cat <<'EOF'
Usage: scripts/maruko_direct_deploy.sh [options] [command] [args]

Direct Maruko deploy-and-test helper for the /etc/waybeam.json workflow.
Defaults to host root@192.168.2.12.

Commands:
  cycle                 Build, backup config, stop waybeam, deploy binary
                        (+ libs/drivers/isp-bins/json_cli if requested),
                        start, wait, status
  full                  cycle + push libs, drivers, isp-bins, json_cli
                        (fresh device)
  build                 Build local Maruko binary only
  backup-config         Copy /etc/waybeam.json to a timestamped backup on target
  restore-config [SRC]  Restore /etc/waybeam.json from SRC or latest backup
  deploy                Copy local binary to /usr/bin/waybeam on target
  push-libs             Push vendor-libs/maruko/*.so → /usr/lib/
                        (also creates uClibc compat symlinks)
  push-drivers          Push sensors/maruko/*.ko → kernel modules dir
                        (use --reboot-after to apply)
  push-isp-bin [NAME]   Push iq-profiles/maruko-bin/[NAME.bin or all *.bin]
                        → /etc/sensors/
  push-json-cli         Build (if needed) and push out/maruko/json_cli
                        → /usr/bin/json_cli
  stop                  Stop running waybeam
  start                 Start waybeam as a daemon and log to /tmp/waybeam.log
  reload                Send SIGHUP to running waybeam
  reboot                Reboot the target device
  wait-http             Poll /api/v1/version until HTTP is ready
  status                Show process/config summary, version, AE, recent log
  config-get PATH       Read a JSON config path with json_cli
  config-set PATH JSON  Set a JSON config path with json_cli

Options:
  --host HOST           SSH target (default: root@192.168.2.12)
  --local-bin PATH      Local binary path (default: out/maruko/waybeam)
  --remote-bin PATH     Remote install path (default: /usr/bin/waybeam)
  --config-path PATH    Remote config path (default: /etc/waybeam.json)
  --log-path PATH       Remote log path (default: /tmp/waybeam.log)
  --backup-path PATH    Explicit remote backup path for backup/restore
  --http-port PORT      Override HTTP port; otherwise read from config
  --wait-secs SECS      HTTP wait timeout in seconds (default: 20)
  --tail-lines N        Log lines for status output (default: 120)
  --skip-build          Skip local build during cycle
  --skip-backup         Skip config backup during cycle
  --with-libs           cycle: also push MI vendor libs
  --with-drivers        cycle: also push sensor .ko (implies --reboot-after)
  --with-isp-bins       cycle: also push ISP .bin blobs
  --with-json-cli       cycle: also push out/maruko/json_cli (built if missing)
  --reboot-after        push-drivers: reboot after install
  --push-config FILE    full: also push FILE to remote /etc/waybeam.json
  --help                Show this help

Examples:
  scripts/maruko_direct_deploy.sh cycle
  scripts/maruko_direct_deploy.sh full                # fresh device bring-up
  scripts/maruko_direct_deploy.sh push-libs
  scripts/maruko_direct_deploy.sh push-drivers --reboot-after
  scripts/maruko_direct_deploy.sh push-isp-bin imx415
  scripts/maruko_direct_deploy.sh config-set .system.verbose true
EOF
}

log() { printf '[maruko_direct] %s\n' "$*"; }
warn() { printf '[maruko_direct] WARNING: %s\n' "$*" >&2; }
die() { printf '[maruko_direct] ERROR: %s\n' "$*" >&2; exit 1; }

remote_sh() {
	local script="$1"
	ssh -o BatchMode=yes -o ConnectTimeout=10 "${HOST}" "sh -c $(printf '%q' "${script}")"
}

remote_capture() {
	local marker="__CODEX_CAPTURE_${RANDOM}_$$__"
	local script="$1"

	remote_sh "printf '%s\n' $(printf '%q' "${marker}"); ${script}; printf '%s\n' $(printf '%q' "${marker}")" |
		awk -v marker="${marker}" '
			index($0, marker) {
				if (capture) { exit }
				capture = 1
				next
			}
			capture { print }
		'
}

push_stream() {
	# push_stream LOCAL_PATH REMOTE_PATH
	local src="$1" dst="$2"
	ssh -o BatchMode=yes -o ConnectTimeout=10 "${HOST}" "cat > $(printf '%q' "${dst}")" < "${src}"
}

require_local_bin() {
	local path="${LOCAL_BIN}"
	if [[ "${path}" != /* ]]; then path="${ROOT_DIR}/${path}"; fi
	[[ -f "${path}" ]] || die "local binary not found: ${path}"
	LOCAL_BIN="${path}"
}

require_remote_tools() {
	remote_sh "command -v json_cli >/dev/null 2>&1 && command -v wget >/dev/null 2>&1"
}

detect_http_port() {
	local port
	if [[ -n "${HTTP_PORT}" ]]; then printf '%s\n' "${HTTP_PORT}"; return 0; fi
	set +e
	port="$(remote_capture "json_cli -g .system.webPort --raw -i $(printf '%q' "${CONFIG_PATH}") 2>/dev/null" | grep -o '[0-9][0-9]*' | tail -n 1 | tr -d '\r\n')"
	set -e
	[[ -z "${port}" ]] && port="80"
	printf '%s\n' "${port}"
}

config_value() {
	local path="$1" fallback="${2:-unknown}" value
	set +e
	value="$(remote_capture "json_cli -g $(printf '%q' "${path}") --raw -i $(printf '%q' "${CONFIG_PATH}") 2>/dev/null || echo $(printf '%q' "${fallback}")" | tail -n 1 | tr -d '\r')"
	set -e
	[[ -z "${value}" ]] && value="${fallback}"
	printf '%s\n' "${value}"
}

build_maruko() {
	log "Building Maruko binary..."
	make -C "${ROOT_DIR}" build SOC_BUILD=maruko
}

create_backup() {
	local stamp target
	if [[ -n "${BACKUP_PATH}" ]]; then
		target="${BACKUP_PATH}"
	else
		stamp="$(date +%Y%m%d-%H%M%S)"
		target="/tmp/waybeam.json.bak.${stamp}"
	fi
	remote_sh "cp $(printf '%q' "${CONFIG_PATH}") $(printf '%q' "${target}") && cp $(printf '%q' "${CONFIG_PATH}") $(printf '%q' "${LATEST_BACKUP_PATH}")"
	log "Backed up ${CONFIG_PATH} -> ${target}"
}

# Provision /etc/waybeam.json on devices that don't yet have one.  Two
# entry paths:
#   1. Upgrade from a venc-era device — /etc/venc.json exists.  Rename
#      it (preserving operator customizations) and clean up the rest of
#      the legacy install so the next reboot doesn't double-start.
#   2. Fresh firstboot — neither path exists.  Write the bundled
#      Maruko default.
# Existing /etc/waybeam.json is left untouched.
provision_default_config_if_missing() {
	local default_cfg="${ROOT_DIR}/config/waybeam.default.maruko.json"
	if remote_capture "[ -f $(printf '%q' "${CONFIG_PATH}") ] && echo PRESENT || echo ABSENT" | grep -q PRESENT; then
		return 0
	fi
	if remote_capture "[ -f /etc/venc.json ] && echo PRESENT || echo ABSENT" | grep -q PRESENT; then
		log "Migrating legacy /etc/venc.json -> ${CONFIG_PATH} (preserving customizations)"
		remote_sh "mv /etc/venc.json $(printf '%q' "${CONFIG_PATH}")"
		remote_sh "rm -f /etc/init.d/S95venc /usr/bin/venc /tmp/venc.log"
		return 0
	fi
	if [[ ! -f "${default_cfg}" ]]; then
		warn "default config ${default_cfg} not found — skipping fresh-device provisioning"
		return 0
	fi
	log "No ${CONFIG_PATH} on device — provisioning default from $(basename "${default_cfg}")"
	push_stream "${default_cfg}" "${CONFIG_PATH}"
}

restore_backup() {
	local source="${1:-${BACKUP_PATH:-${LATEST_BACKUP_PATH}}}"
	remote_sh "cp $(printf '%q' "${source}") $(printf '%q' "${CONFIG_PATH}")"
	log "Restored ${CONFIG_PATH} from ${source}"
}

stop_venc() {
	log "Stopping waybeam..."
	remote_capture "killall waybeam 2>/dev/null; sleep 2; ps w | grep -E '(^|/)waybeam( |\$)' | grep -v grep || true" >/dev/null
}

deploy_init_script() {
	local local_path="${INIT_SCRIPT_LOCAL}"
	if [[ "${local_path}" != /* ]]; then local_path="${ROOT_DIR}/${local_path}"; fi
	[[ -f "${local_path}" ]] || die "init script missing: ${local_path}"
	log "Deploying ${local_path} -> ${HOST}:${INIT_SCRIPT_REMOTE}"
	ssh -o BatchMode=yes -o ConnectTimeout=10 "${HOST}" "cat > $(printf '%q' "${INIT_SCRIPT_REMOTE}")" < "${local_path}"
	remote_sh "chmod 0755 $(printf '%q' "${INIT_SCRIPT_REMOTE}")"
}

deploy_binary() {
	local remote_tmp
	require_local_bin
	remote_tmp="${REMOTE_BIN}.codex.new"
	log "Deploying ${LOCAL_BIN} -> ${HOST}:${REMOTE_BIN}"
	push_stream "${LOCAL_BIN}" "${remote_tmp}"
	remote_sh "chmod 0755 $(printf '%q' "${remote_tmp}") && mv $(printf '%q' "${remote_tmp}") $(printf '%q' "${REMOTE_BIN}")"
}

push_libs() {
	[[ -d "${LIBS_DIR}" ]] || die "missing ${LIBS_DIR} — run scripts/maruko_pull_artifacts.sh first"
	local count=0 f base
	log "Pushing MI vendor libs from ${LIBS_DIR} -> ${HOST}:${REMOTE_LIB_DIR}"
	shopt -s nullglob
	for f in "${LIBS_DIR}"/*.so; do
		base="$(basename "${f}")"
		push_stream "${f}" "${REMOTE_LIB_DIR}/${base}"
		count=$((count + 1))
	done
	shopt -u nullglob
	[[ "${count}" -gt 0 ]] || warn "no .so files under ${LIBS_DIR}"
	# libcam_os_wrapper.so has hardcoded NEEDED tags ld-uClibc.so.1 and
	# libc.so.0 (vendor blob; cannot be relinked).  Stock OpenIPC musl
	# firmware ships /lib/libc.so only — create the missing symlinks so
	# the dynamic loader can resolve the wrapper's deps.  Idempotent.
	remote_sh "
		cd /lib &&
		[ -e ld-uClibc.so.1 ] || ln -sf libc.so ld-uClibc.so.1
		[ -e libc.so.0 ] || ln -sf libc.so libc.so.0
		ldconfig 2>/dev/null || true
	"
	log "Pushed ${count} libs (+ uClibc compat symlinks)"
}

push_drivers() {
	[[ -d "${KO_DIR}" ]] || die "missing ${KO_DIR} — run scripts/maruko_pull_artifacts.sh first"
	local count=0 skipped=0 f base remote
	log "Pushing sensor .ko from ${KO_DIR} -> ${HOST}:${REMOTE_KO_DIR}"
	remote_sh "mkdir -p $(printf '%q' "${REMOTE_KO_DIR}")"
	shopt -s nullglob
	# Two-pass: source-built (sensor_*_maruko.ko) win over pulled blobs
	# (sensor_*_mipi.ko) when both exist for the same sensor.  Glob order
	# is alphabetical, so without this skip the pulled blob would clobber
	# the source-built rename on disk.
	for f in "${KO_DIR}"/*.ko; do
		base="$(basename "${f}")"
		case "${base}" in
			sensor_*_maruko.ko)
				remote="${base%_maruko.ko}_mipi.ko"
				;;
			sensor_*_mipi.ko)
				# If a source-built sibling exists, prefer it.
				local sibling="${KO_DIR}/${base%_mipi.ko}_maruko.ko"
				if [[ -f "${sibling}" ]]; then
					skipped=$((skipped + 1))
					continue
				fi
				remote="${base}"
				;;
			*)
				remote="${base}"
				;;
		esac
		push_stream "${f}" "${REMOTE_KO_DIR}/${remote}"
		count=$((count + 1))
	done
	shopt -u nullglob
	[[ "${count}" -gt 0 ]] || warn "no .ko files under ${KO_DIR}"
	if [[ "${skipped}" -gt 0 ]]; then
		log "Pushed ${count} drivers (${skipped} pulled _mipi.ko skipped in favor of source-built _maruko.ko)"
	else
		log "Pushed ${count} drivers"
	fi
	if [[ "${REBOOT_AFTER_DRIVERS}" -eq 1 ]]; then
		log "Rebooting target to load new kernel modules..."
		remote_sh "reboot" || true
	else
		log "Reboot required before new drivers take effect (use --reboot-after)"
	fi
}

push_json_cli() {
	# Build on demand so a stale repo without out/maruko/json_cli still works.
	if [[ ! -f "${JSON_CLI_LOCAL}" ]]; then
		log "Building json_cli for maruko..."
		make -C "${ROOT_DIR}" json_cli SOC_BUILD=maruko >/dev/null
	fi
	[[ -f "${JSON_CLI_LOCAL}" ]] || die "build did not produce ${JSON_CLI_LOCAL}"
	local remote_tmp="${JSON_CLI_REMOTE}.codex.new"
	log "Deploying ${JSON_CLI_LOCAL} -> ${HOST}:${JSON_CLI_REMOTE}"
	push_stream "${JSON_CLI_LOCAL}" "${remote_tmp}"
	remote_sh "chmod 0755 $(printf '%q' "${remote_tmp}") && mv $(printf '%q' "${remote_tmp}") $(printf '%q' "${JSON_CLI_REMOTE}")"
}

push_isp_bin() {
	local target="${1:-}"
	[[ -d "${ISP_BIN_DIR}" ]] || die "missing ${ISP_BIN_DIR} — run scripts/maruko_pull_artifacts.sh first"
	remote_sh "mkdir -p $(printf '%q' "${REMOTE_ISP_BIN_DIR}")"
	local count=0 f base
	shopt -s nullglob
	if [[ -n "${target}" ]]; then
		# Allow short form (imx415) or full filename (imx415.bin)
		[[ "${target}" == *.bin ]] || target="${target}.bin"
		f="${ISP_BIN_DIR}/${target}"
		[[ -f "${f}" ]] || die "no such ISP bin: ${f}"
		push_stream "${f}" "${REMOTE_ISP_BIN_DIR}/${target}"
		count=1
	else
		for f in "${ISP_BIN_DIR}"/*.bin; do
			base="$(basename "${f}")"
			push_stream "${f}" "${REMOTE_ISP_BIN_DIR}/${base}"
			count=$((count + 1))
		done
	fi
	shopt -u nullglob
	[[ "${count}" -gt 0 ]] || warn "no ISP .bin pushed"
	log "Pushed ${count} ISP bin(s) to ${REMOTE_ISP_BIN_DIR}"
}

start_venc() {
	log "Starting waybeam with log ${LOG_PATH}"
	remote_sh "
		if command -v setsid >/dev/null 2>&1; then
			setsid $(printf '%q' "${REMOTE_BIN}") >$(printf '%q' "${LOG_PATH}") 2>&1 </dev/null &
		else
			nohup $(printf '%q' "${REMOTE_BIN}") >$(printf '%q' "${LOG_PATH}") 2>&1 </dev/null &
		fi
	"
}

reload_venc() {
	log "Sending SIGHUP to waybeam..."
	remote_sh "killall -HUP waybeam"
}

reboot_device() {
	log "Rebooting ${HOST}..."
	remote_sh "reboot" || true
}

wait_http() {
	local port deadline
	require_remote_tools
	port="$(detect_http_port)"
	log "Waiting for HTTP on port ${port} (${WAIT_SECS}s timeout)"
	deadline=$((SECONDS + WAIT_SECS))
	while (( SECONDS < deadline )); do
		if remote_sh "wget -q -O- http://127.0.0.1:${port}/api/v1/version >/dev/null 2>&1"; then
			log "HTTP ready on port ${port}"
			return 0
		fi
		sleep 1
	done
	die "HTTP did not become ready within ${WAIT_SECS}s"
}

show_status() {
	local port
	require_remote_tools
	port="$(detect_http_port)"

	log "Process"
	remote_capture "ps w | grep -E '(^|/)waybeam( |\$)' | grep -v grep || true"

	log "Config summary"
	printf 'webPort=%s\n'        "$(config_value .system.webPort 80)"
	printf 'verbose=%s\n'        "$(config_value .system.verbose unknown)"
	printf 'sensor.index=%s\n'   "$(config_value .sensor.index unknown)"
	printf 'sensor.mode=%s\n'    "$(config_value .sensor.mode unknown)"
	printf 'isp.sensorBin=%s\n'  "$(config_value .isp.sensorBin unknown)"
	printf 'isp.aeMode=%s\n'     "$(config_value .isp.aeMode unknown)"
	printf 'video0.size=%s\n'    "$(config_value .video0.size unknown)"
	printf 'video0.fps=%s\n'     "$(config_value .video0.fps unknown)"
	printf 'video0.bitrate=%s\n' "$(config_value .video0.bitrate unknown)"

	log "Version endpoint"
	remote_capture "wget -q -O- http://127.0.0.1:${port}/api/v1/version || true"

	log "AE endpoint"
	remote_capture "wget -q -O- http://127.0.0.1:${port}/api/v1/ae || true"

	log "Recent log (${LOG_PATH})"
	remote_capture "tail -n $(printf '%q' "${TAIL_LINES}") $(printf '%q' "${LOG_PATH}") 2>/dev/null || true"
}

config_get() {
	local path="${1:-}"
	[[ -n "${path}" ]] || die "config-get requires a JSON path"
	require_remote_tools
	remote_capture "json_cli -g $(printf '%q' "${path}") -i $(printf '%q' "${CONFIG_PATH}")"
}

config_set() {
	local path="${1:-}" value="${2:-}"
	[[ -n "${path}" ]]  || die "config-set requires a JSON path"
	[[ -n "${value}" ]] || die "config-set requires a JSON value"
	require_remote_tools
	remote_sh "json_cli -s $(printf '%q' "${path}") $(printf '%q' "${value}") -i $(printf '%q' "${CONFIG_PATH}")"
	log "Updated ${path}"
}

run_cycle() {
	if [[ "${SKIP_BUILD}" -eq 0 ]]; then
		build_maruko
	else
		require_local_bin
		log "Skipping build"
	fi

	stop_venc

	provision_default_config_if_missing

	if [[ "${SKIP_BACKUP}" -eq 0 ]]; then
		create_backup
	else
		log "Skipping config backup"
	fi

	deploy_binary
	deploy_init_script
	[[ "${WITH_LIBS}" -eq 1 ]]     && push_libs
	[[ "${WITH_JSON_CLI}" -eq 1 ]] && push_json_cli
	[[ "${WITH_DRIVERS}" -eq 1 ]]  && push_drivers
	[[ "${WITH_ISP_BINS}" -eq 1 ]] && push_isp_bin
	start_venc
	wait_http
	show_status
}

run_full() {
	WITH_LIBS=1
	WITH_DRIVERS=1
	WITH_ISP_BINS=1
	WITH_JSON_CLI=1
	# Drivers need a reboot before the next waybeam start can pick them up.
	REBOOT_AFTER_DRIVERS=1
	if [[ "${SKIP_BUILD}" -eq 0 ]]; then
		build_maruko
	else
		log "SKIP_BUILD=1 — using existing ${LOCAL_BIN}"
	fi
	require_local_bin
	[[ -f "${JSON_CLI_LOCAL}" ]] || make -C "${ROOT_DIR}" json_cli SOC_BUILD=maruko >/dev/null
	create_backup || true
	stop_venc
	bulk_push_all
	if [[ -n "${PUSH_CONFIG_FILE}" ]]; then
		[[ -f "${PUSH_CONFIG_FILE}" ]] || die "config file not found: ${PUSH_CONFIG_FILE}"
		log "Pushing ${PUSH_CONFIG_FILE} -> ${HOST}:${CONFIG_PATH}"
		push_stream "${PUSH_CONFIG_FILE}" "${CONFIG_PATH}"
	else
		provision_default_config_if_missing
	fi
	log "Rebooting target to load new kernel modules..."
	remote_sh "reboot" || true
	log "Device rebooting; not waiting for HTTP. Re-run 'cycle' or 'status' once it returns."
}

# Bulk push: waybeam binary + json_cli + MI vendor libs + sensor .ko + ISP .bin
# in a single tar | ssh pipe.  Single SSH handshake → ~10× faster than the
# per-file scp loop (verified 2026-05-14: 14 libs + 10 .ko + 3 bins + 2
# binaries dropped from ~60s to ~7s on the bench at 192.168.2.12).
#
# Driver renaming (sensor_<name>_maruko.ko → sensor_<name>_mipi.ko, with
# pulled _mipi.ko skipped when a source-built _maruko.ko sibling exists)
# happens at staging time, not on the remote, so the extract is a plain
# `tar -xf -` into /.  Uses `cp` (not `install`) on the remote — busybox
# does not ship `install`.
bulk_push_all() {
	[[ -d "${LIBS_DIR}" ]] || die "missing ${LIBS_DIR} — run scripts/maruko_pull_artifacts.sh first"
	[[ -d "${KO_DIR}" ]] || die "missing ${KO_DIR} — run scripts/maruko_pull_artifacts.sh first"
	[[ -d "${ISP_BIN_DIR}" ]] || die "missing ${ISP_BIN_DIR} — run scripts/maruko_pull_artifacts.sh first"

	local stage
	stage="$(mktemp -d "${TMPDIR:-/tmp}/maruko_deploy.XXXXXX")"
	trap "rm -rf '${stage}'" RETURN

	mkdir -p \
		"${stage}/usr/bin" \
		"${stage}/usr/lib" \
		"${stage}/etc/init.d" \
		"${stage}${REMOTE_KO_DIR}" \
		"${stage}${REMOTE_ISP_BIN_DIR}"

	# waybeam binary + json_cli + init script
	cp "${LOCAL_BIN}" "${stage}/usr/bin/waybeam"
	chmod 0755 "${stage}/usr/bin/waybeam"
	cp "${JSON_CLI_LOCAL}" "${stage}/usr/bin/json_cli"
	chmod 0755 "${stage}/usr/bin/json_cli"
	local init_src="${INIT_SCRIPT_LOCAL}"
	if [[ "${init_src}" != /* ]]; then init_src="${ROOT_DIR}/${init_src}"; fi
	if [[ -f "${init_src}" ]]; then
		cp "${init_src}" "${stage}${INIT_SCRIPT_REMOTE}"
		chmod 0755 "${stage}${INIT_SCRIPT_REMOTE}"
	else
		warn "init script ${init_src} missing — skipping"
	fi

	# MI vendor libs
	local libcount=0 f base
	shopt -s nullglob
	for f in "${LIBS_DIR}"/*.so; do
		base="$(basename "${f}")"
		cp "${f}" "${stage}/usr/lib/${base}"
		libcount=$((libcount + 1))
	done

	# Sensor .ko — source-built _maruko.ko renamed to _mipi.ko;
	# pulled _mipi.ko skipped when source-built sibling exists.
	local kocount=0 skipped=0 remote sibling
	for f in "${KO_DIR}"/*.ko; do
		base="$(basename "${f}")"
		case "${base}" in
			sensor_*_maruko.ko)
				remote="${base%_maruko.ko}_mipi.ko"
				;;
			sensor_*_mipi.ko)
				sibling="${KO_DIR}/${base%_mipi.ko}_maruko.ko"
				if [[ -f "${sibling}" ]]; then
					skipped=$((skipped + 1))
					continue
				fi
				remote="${base}"
				;;
			*)
				remote="${base}"
				;;
		esac
		cp "${f}" "${stage}${REMOTE_KO_DIR}/${remote}"
		kocount=$((kocount + 1))
	done

	# ISP bins
	local bincount=0
	for f in "${ISP_BIN_DIR}"/*.bin; do
		base="$(basename "${f}")"
		cp "${f}" "${stage}${REMOTE_ISP_BIN_DIR}/${base}"
		bincount=$((bincount + 1))
	done
	shopt -u nullglob

	log "Bulk pushing: waybeam + json_cli + ${libcount} libs + ${kocount} .ko + ${bincount} ISP bins (single ssh)"
	(cd "${stage}" && tar -cf - .) | \
		ssh -o BatchMode=yes -o ConnectTimeout=10 "${HOST}" "tar -xf - -C /"

	# uClibc compat symlinks (idempotent — same as push_libs)
	remote_sh "
		cd /lib &&
		[ -e ld-uClibc.so.1 ] || ln -sf libc.so ld-uClibc.so.1
		[ -e libc.so.0 ] || ln -sf libc.so libc.so.0
		ldconfig 2>/dev/null || true
	"
	if [[ "${skipped}" -gt 0 ]]; then
		log "Pushed: waybeam + json_cli + ${libcount} libs + ${kocount} .ko (${skipped} pulled _mipi.ko skipped for source-built sibling) + ${bincount} ISP bins"
	else
		log "Pushed: waybeam + json_cli + ${libcount} libs + ${kocount} .ko + ${bincount} ISP bins"
	fi
}

POSITIONAL=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--host)              HOST="$2"; shift 2 ;;
		--local-bin)         LOCAL_BIN="$2"; shift 2 ;;
		--remote-bin)        REMOTE_BIN="$2"; shift 2 ;;
		--config-path)       CONFIG_PATH="$2"; shift 2 ;;
		--log-path)          LOG_PATH="$2"; shift 2 ;;
		--backup-path)       BACKUP_PATH="$2"; shift 2 ;;
		--http-port)         HTTP_PORT="$2"; shift 2 ;;
		--wait-secs)         WAIT_SECS="$2"; shift 2 ;;
		--tail-lines)        TAIL_LINES="$2"; shift 2 ;;
		--skip-build)        SKIP_BUILD=1; shift ;;
		--skip-backup)       SKIP_BACKUP=1; shift ;;
		--with-libs)         WITH_LIBS=1; shift ;;
		--with-drivers)      WITH_DRIVERS=1; REBOOT_AFTER_DRIVERS=1; shift ;;
		--with-isp-bins)     WITH_ISP_BINS=1; shift ;;
		--with-json-cli)     WITH_JSON_CLI=1; shift ;;
		--reboot-after)      REBOOT_AFTER_DRIVERS=1; shift ;;
		--push-config)       PUSH_CONFIG_FILE="$2"; shift 2 ;;
		--help|-h)           usage; exit 0 ;;
		--*)                 die "unknown option: $1" ;;
		*)                   POSITIONAL+=("$1"); shift ;;
	esac
done

# Positional args: first is COMMAND, rest are COMMAND_ARGS.  Parsing options
# in any order (not strictly before the command) is what the user expects
# from `scripts/... full --skip-build --push-config FILE` — the prior
# `break`-on-command parser silently swallowed those.
if [[ "${#POSITIONAL[@]}" -gt 0 ]]; then
	COMMAND="${POSITIONAL[0]}"
	COMMAND_ARGS=("${POSITIONAL[@]:1}")
fi
case "${COMMAND}" in
	cycle|full|build|backup-config|restore-config|deploy|push-libs|push-drivers|push-isp-bin|push-json-cli|stop|start|reload|reboot|wait-http|status|config-get|config-set) ;;
	*) die "unknown command: ${COMMAND}" ;;
esac

case "${COMMAND}" in
	cycle)          run_cycle ;;
	full)           run_full ;;
	build)          build_maruko ;;
	backup-config)  create_backup ;;
	restore-config) restore_backup "${COMMAND_ARGS[0]:-}" ;;
	deploy)         deploy_binary; deploy_init_script ;;
	push-libs)      push_libs ;;
	push-drivers)   push_drivers ;;
	push-isp-bin)   push_isp_bin "${COMMAND_ARGS[0]:-}" ;;
	push-json-cli)  push_json_cli ;;
	stop)           stop_venc ;;
	start)          start_venc ;;
	reload)         reload_venc ;;
	reboot)         reboot_device ;;
	wait-http)      wait_http ;;
	status)         show_status ;;
	config-get)     config_get "${COMMAND_ARGS[0]:-}" ;;
	config-set)     config_set "${COMMAND_ARGS[0]:-}" "${COMMAND_ARGS[1]:-}" ;;
	*)              die "unsupported command: ${COMMAND}" ;;
esac
