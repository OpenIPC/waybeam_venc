#!/usr/bin/env bash
set -euo pipefail

HOST="${HOST:-root@192.168.1.13}"
LOCAL_BIN="out/star6e/venc"
REMOTE_BIN="/usr/bin/venc"
CONFIG_PATH="/etc/venc.json"
LOG_PATH="/tmp/venc.log"
LATEST_BACKUP_PATH="/tmp/venc.json.bak.latest"
WAIT_SECS=20
TAIL_LINES=120
HTTP_PORT="${HTTP_PORT:-}"
SKIP_BUILD=0
SKIP_BACKUP=0
BACKUP_PATH=""
COMMAND="cycle"
COMMAND_ARGS=()

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
	cat <<'EOF'
Usage: scripts/star6e_direct_deploy.sh [options] [command] [args]

Direct Star6E deploy-and-test helper for current /etc/venc.json workflow.
Defaults to host root@192.168.1.13.

Commands:
  cycle                 Build, backup config, stop venc, deploy, start, wait, status
  build                 Build local Star6E binary only
  backup-config         Copy /etc/venc.json to a timestamped backup on target
  restore-config [SRC]  Restore /etc/venc.json from SRC or latest backup
  deploy                Copy local binary to /usr/bin/venc on target
  stop                  Stop running venc
  start                 Start venc as a daemon and log to /tmp/venc.log
  reload                Send SIGHUP to running venc
  wait-http             Poll /api/v1/version until HTTP is ready
  status                Show process/config summary, version, AE, and recent log
  config-get PATH       Read a JSON config path with json_cli
  config-set PATH JSON  Set a JSON config path with json_cli

Options:
  --host HOST           SSH target (default: root@192.168.1.13)
  --local-bin PATH      Local binary path (default: out/star6e/venc)
  --remote-bin PATH     Remote install path (default: /usr/bin/venc)
  --config-path PATH    Remote config path (default: /etc/venc.json)
  --log-path PATH       Remote log path (default: /tmp/venc.log)
  --backup-path PATH    Explicit remote backup path for backup/restore
  --http-port PORT      Override HTTP port; otherwise read from config
  --wait-secs SECS      HTTP wait timeout in seconds (default: 20)
  --tail-lines N        Log lines for status output (default: 120)
  --skip-build          Skip local build during cycle
  --skip-backup         Skip config backup during cycle
  --help                Show this help

Examples:
  scripts/star6e_direct_deploy.sh cycle
  scripts/star6e_direct_deploy.sh config-set .system.verbose true
  scripts/star6e_direct_deploy.sh config-set .isp.sensorBin '"/etc/sensors/imx335_greg_fpvVII-gpt200.bin"'
  scripts/star6e_direct_deploy.sh reload
  scripts/star6e_direct_deploy.sh status
EOF
}

log() {
	printf '[star6e_direct] %s\n' "$*"
}

die() {
	printf '[star6e_direct] ERROR: %s\n' "$*" >&2
	exit 1
}

remote_sh() {
	local script="$1"
	ssh -o BatchMode=yes -o ConnectTimeout=5 "${HOST}" "sh -lc $(printf '%q' "${script}")"
}

remote_capture() {
	local marker="__CODEX_CAPTURE_${RANDOM}_$$__"
	local script="$1"

	remote_sh "printf '%s\n' $(printf '%q' "${marker}"); ${script}; printf '%s\n' $(printf '%q' "${marker}")" |
		awk -v marker="${marker}" '
			index($0, marker) {
				if (capture) {
					exit
				}
				capture = 1
				next
			}
			capture {
				print
			}
		'
}

require_local_bin() {
	local path

	path="${LOCAL_BIN}"
	if [[ "${path}" != /* ]]; then
		path="${ROOT_DIR}/${path}"
	fi
	if [[ ! -f "${path}" ]]; then
		die "local binary not found: ${path}"
	fi
	LOCAL_BIN="${path}"
}

require_remote_tools() {
	remote_sh "command -v json_cli >/dev/null 2>&1 && command -v wget >/dev/null 2>&1"
}

detect_http_port() {
	local port

	if [[ -n "${HTTP_PORT}" ]]; then
		printf '%s\n' "${HTTP_PORT}"
		return 0
	fi

	set +e
	port="$(remote_capture "json_cli -g .system.webPort --raw -i $(printf '%q' "${CONFIG_PATH}") 2>/dev/null" | grep -o '[0-9][0-9]*' | tail -n 1 | tr -d '\r\n')"
	set -e
	if [[ -z "${port}" ]]; then
		port="80"
	fi
	printf '%s\n' "${port}"
}

config_value() {
	local path="$1"
	local fallback="${2:-unknown}"
	local value

	set +e
	value="$(remote_capture "json_cli -g $(printf '%q' "${path}") --raw -i $(printf '%q' "${CONFIG_PATH}") 2>/dev/null || echo $(printf '%q' "${fallback}")" | tail -n 1 | tr -d '\r')"
	set -e
	if [[ -z "${value}" ]]; then
		value="${fallback}"
	fi
	printf '%s\n' "${value}"
}

build_star6e() {
	log "Building Star6E binary..."
	make -C "${ROOT_DIR}" build SOC_BUILD=star6e
}

create_backup() {
	local stamp target

	if [[ -n "${BACKUP_PATH}" ]]; then
		target="${BACKUP_PATH}"
	else
		stamp="$(date +%Y%m%d-%H%M%S)"
		target="/tmp/venc.json.bak.${stamp}"
	fi

	remote_sh "cp $(printf '%q' "${CONFIG_PATH}") $(printf '%q' "${target}") && cp $(printf '%q' "${CONFIG_PATH}") $(printf '%q' "${LATEST_BACKUP_PATH}")"
	log "Backed up ${CONFIG_PATH} -> ${target}"
}

restore_backup() {
	local source="${1:-${BACKUP_PATH:-${LATEST_BACKUP_PATH}}}"

	remote_sh "cp $(printf '%q' "${source}") $(printf '%q' "${CONFIG_PATH}")"
	log "Restored ${CONFIG_PATH} from ${source}"
}

stop_venc() {
	log "Stopping venc..."
	remote_capture "killall venc 2>/dev/null || true; sleep 2; ps w | grep -E '(^|/)venc( |$)' | grep -v grep || true" >/dev/null
}

deploy_binary() {
	local remote_tmp

	require_local_bin
	remote_tmp="${REMOTE_BIN}.codex.new"
	log "Deploying ${LOCAL_BIN} -> ${HOST}:${REMOTE_BIN}"
	ssh -o BatchMode=yes -o ConnectTimeout=5 "${HOST}" "cat > $(printf '%q' "${remote_tmp}")" < "${LOCAL_BIN}"
	remote_sh "chmod 0755 $(printf '%q' "${remote_tmp}") && mv $(printf '%q' "${remote_tmp}") $(printf '%q' "${REMOTE_BIN}")"
}

start_venc() {
	log "Starting venc with log ${LOG_PATH}"
	remote_sh "
		if command -v setsid >/dev/null 2>&1; then
			setsid $(printf '%q' "${REMOTE_BIN}") >$(printf '%q' "${LOG_PATH}") 2>&1 </dev/null &
		else
			nohup $(printf '%q' "${REMOTE_BIN}") >$(printf '%q' "${LOG_PATH}") 2>&1 </dev/null &
		fi
	"
}

reload_venc() {
	log "Sending SIGHUP to venc..."
	remote_sh "killall -HUP venc"
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
	remote_capture "ps w | grep -E '(^|/)venc( |$)' | grep -v grep || true"

	log "Config summary"
	printf 'webPort=%s\n' "$(config_value .system.webPort 80)"
	printf 'verbose=%s\n' "$(config_value .system.verbose unknown)"
	printf 'sensor.index=%s\n' "$(config_value .sensor.index unknown)"
	printf 'sensor.mode=%s\n' "$(config_value .sensor.mode unknown)"
	printf 'isp.sensorBin=%s\n' "$(config_value .isp.sensorBin unknown)"
	printf 'video0.size=%s\n' "$(config_value .video0.size unknown)"
	printf 'video0.fps=%s\n' "$(config_value .video0.fps unknown)"
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
	local path="${1:-}"
	local value="${2:-}"

	[[ -n "${path}" ]] || die "config-set requires a JSON path"
	[[ -n "${value}" ]] || die "config-set requires a JSON value"
	require_remote_tools
	remote_sh "json_cli -s $(printf '%q' "${path}") $(printf '%q' "${value}") -i $(printf '%q' "${CONFIG_PATH}")"
	log "Updated ${path}"
}

run_cycle() {
	if [[ "${SKIP_BUILD}" -eq 0 ]]; then
		build_star6e
	else
		require_local_bin
		log "Skipping build"
	fi

	if [[ "${SKIP_BACKUP}" -eq 0 ]]; then
		create_backup
	else
		log "Skipping config backup"
	fi

	stop_venc
	deploy_binary
	start_venc
	wait_http
	show_status
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		cycle|build|backup-config|restore-config|deploy|stop|start|reload|wait-http|status|config-get|config-set)
			COMMAND="$1"
			shift
			COMMAND_ARGS=("$@")
			break
			;;
		--host)
			HOST="$2"
			shift 2
			;;
		--local-bin)
			LOCAL_BIN="$2"
			shift 2
			;;
		--remote-bin)
			REMOTE_BIN="$2"
			shift 2
			;;
		--config-path)
			CONFIG_PATH="$2"
			shift 2
			;;
		--log-path)
			LOG_PATH="$2"
			shift 2
			;;
		--backup-path)
			BACKUP_PATH="$2"
			shift 2
			;;
		--http-port)
			HTTP_PORT="$2"
			shift 2
			;;
		--wait-secs)
			WAIT_SECS="$2"
			shift 2
			;;
		--tail-lines)
			TAIL_LINES="$2"
			shift 2
			;;
		--skip-build)
			SKIP_BUILD=1
			shift
			;;
		--skip-backup)
			SKIP_BACKUP=1
			shift
			;;
		--help|-h)
			usage
			exit 0
			;;
		*)
			die "unknown option or command: $1"
			;;
	esac
done

case "${COMMAND}" in
	cycle)
		run_cycle
		;;
	build)
		build_star6e
		;;
	backup-config)
		create_backup
		;;
	restore-config)
		restore_backup "${COMMAND_ARGS[0]:-}"
		;;
	deploy)
		deploy_binary
		;;
	stop)
		stop_venc
		;;
	start)
		start_venc
		;;
	reload)
		reload_venc
		;;
	wait-http)
		wait_http
		;;
	status)
		show_status
		;;
	config-get)
		config_get "${COMMAND_ARGS[0]:-}"
		;;
	config-set)
		config_set "${COMMAND_ARGS[0]:-}" "${COMMAND_ARGS[1]:-}"
		;;
	*)
		die "unsupported command: ${COMMAND}"
		;;
esac
