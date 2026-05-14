#!/usr/bin/env bash
set -euo pipefail

# maruko_sensor_init_diff.sh
#
# Capture sensor register state across four scenarios to find which writes
# majestic performs that venc does not — the symptom is a very dark image
# on firstboot when venc runs cold, but a normal image after majestic has
# touched the sensor first.
#
# Scenarios captured per run:
#   E — current        : sample whatever streamer is already running, no reboot
#                        (use this to lock in a known-good "restored" state
#                        before tearing it down with reboots for A/B/C/D)
#   A — firstboot      : reboot, no streamer started before sampling
#   B — venc cold      : reboot, start venc, settle, sample
#   C — majestic cold  : reboot, start majestic, settle, sample
#   D — majestic→venc  : reboot, run majestic briefly, kill, start venc, sample
#
# Each scenario writes <out>/<id>.regs and (B/C/D only) <id>.jpg + a brightness
# mean.  The script then computes:
#   diff_C_vs_B.txt — what majestic does that venc does not (the smoking gun)
#   diff_D_vs_B.txt — registers that survive from majestic into a venc run
#   diff_B_vs_A.txt / diff_C_vs_A.txt — what each streamer writes from cold
#
# Prerequisites on the target (provisioned by `scripts/maruko_direct_deploy.sh full`):
#   /usr/bin/venc
#   /usr/bin/majestic   (stock OpenIPC)
#   /usr/bin/regscan    (this script pushes it)
#   /usr/lib/lib*.so    (MI vendor libs + uClibc compat symlinks)
#   /lib/modules/.../sensor_imx415_mipi.ko (or imx335)
#   /etc/sensors/<sensor>.bin
#   /etc/venc.json, /etc/majestic.yaml
#
# Usage:
#   scripts/maruko_sensor_init_diff.sh
#   scripts/maruko_sensor_init_diff.sh --host root@192.168.2.12
#   scripts/maruko_sensor_init_diff.sh --i2c-dev /dev/i2c-1 --i2c-addr 0x1a
#   scripts/maruko_sensor_init_diff.sh --out /tmp/sensor-init-$(date +%s)

HOST="${HOST:-root@192.168.2.12}"
I2C_DEV="${I2C_DEV:-auto}"
I2C_ADDR="${I2C_ADDR:-0x1a}"
SETTLE_SECS="${SETTLE_SECS:-6}"
MAJESTIC_BRIEF_SECS="${MAJESTIC_BRIEF_SECS:-4}"
SSH_TIMEOUT="${SSH_TIMEOUT:-10}"
REBOOT_WAIT_SECS="${REBOOT_WAIT_SECS:-90}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_BASE="${OUT_DIR:-${ROOT_DIR}/bench_logs/sensor_init_diff}"
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR=""
SKIP_PUSH=0
MODE="full"      # full | current

usage() {
	cat <<EOF
Usage: scripts/maruko_sensor_init_diff.sh [options]

Compare sensor register state across firstboot / venc-cold /
majestic-cold / majestic-then-venc scenarios on a Maruko target.

Options:
  --host HOST           SSH target (default: ${HOST})
  --i2c-dev DEV         /dev/i2c-N (default: auto-probe 0,1,2)
  --i2c-addr ADDR       Sensor 7-bit addr (default: ${I2C_ADDR})
  --out DIR             Output directory (default: ${OUT_BASE}/<timestamp>/)
  --settle SECS         Seconds to wait after starting a streamer (${SETTLE_SECS})
  --maj-brief SECS      Seconds to leave majestic running in scenario D (${MAJESTIC_BRIEF_SECS})
  --reboot-wait SECS    Max seconds to wait for SSH after reboot (${REBOOT_WAIT_SECS})
  --skip-push           Assume regscan + binaries already on target
  --current-only        Only capture scenario E (live state, no reboots) and exit.
                        Use this to lock in a known-good "restored" stream
                        before tearing the bench down for the full A/B/C/D sweep.
  --help                Show this help

Outputs:
  <out>/A_firstboot.regs
  <out>/B_venc_cold.{regs,jpg,brightness}
  <out>/C_majestic_cold.{regs,jpg,brightness}
  <out>/D_majestic_then_venc.{regs,jpg,brightness}
  <out>/diff_*.txt
  <out>/report.md

Run the script, then read <out>/report.md.  Re-run on the same device
for repeatability — every scenario starts from a fresh reboot.
EOF
}

log()  { printf '[init_diff] %s\n' "$*"; }
warn() { printf '[init_diff] WARNING: %s\n' "$*" >&2; }
die()  { printf '[init_diff] ERROR: %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
	case "$1" in
		--host)         HOST="$2"; shift 2 ;;
		--i2c-dev)      I2C_DEV="$2"; shift 2 ;;
		--i2c-addr)     I2C_ADDR="$2"; shift 2 ;;
		--out)          OUT_DIR="$2"; shift 2 ;;
		--settle)       SETTLE_SECS="$2"; shift 2 ;;
		--maj-brief)    MAJESTIC_BRIEF_SECS="$2"; shift 2 ;;
		--reboot-wait)  REBOOT_WAIT_SECS="$2"; shift 2 ;;
		--skip-push)    SKIP_PUSH=1; shift ;;
		--current-only) MODE="current"; shift ;;
		--help|-h)      usage; exit 0 ;;
		*) die "unknown argument: $1" ;;
	esac
done

[[ -z "${OUT_DIR}" ]] && OUT_DIR="${OUT_BASE}/${TIMESTAMP}"
mkdir -p "${OUT_DIR}"

remote_sh()  { ssh -o BatchMode=yes -o ConnectTimeout="${SSH_TIMEOUT}" "${HOST}" "$@"; }
remote_pull() { ssh -o BatchMode=yes -o ConnectTimeout="${SSH_TIMEOUT}" "${HOST}" "cat $1" ; }
push_stream() {
	local src="$1" dst="$2"
	ssh -o BatchMode=yes -o ConnectTimeout="${SSH_TIMEOUT}" "${HOST}" "cat > $(printf '%q' "${dst}")" < "${src}"
}

http_get() {
	local url="$1" out="$2"
	curl -s -m 10 -o "${out}" "${url}" || true
}

wait_for_ssh() {
	local deadline=$(( $(date +%s) + REBOOT_WAIT_SECS ))
	log "Waiting up to ${REBOOT_WAIT_SECS}s for ${HOST} to come back online..."
	until remote_sh 'echo ok' >/dev/null 2>&1; do
		if [[ $(date +%s) -ge ${deadline} ]]; then
			die "target did not come back within ${REBOOT_WAIT_SECS}s"
		fi
		sleep 2
	done
	log "Target is responsive."
}

probe_i2c() {
	if [[ "${I2C_DEV}" != "auto" ]]; then
		echo "${I2C_DEV}"
		return
	fi
	local dev val
	for dev in /dev/i2c-0 /dev/i2c-1 /dev/i2c-2; do
		val="$(remote_sh "/usr/bin/regscan -d ${dev} -a ${I2C_ADDR} -q 2>/dev/null | awk '/^0x3000/ {print \$3; exit}'" || true)"
		if [[ -n "${val}" && "${val}" != "<read" ]]; then
			echo "${dev}"
			return
		fi
	done
	die "could not find a working i2c-dev (tried 0/1/2 at addr ${I2C_ADDR})"
}

target_ip() {
	# Strip "user@" prefix if present
	echo "${HOST#*@}"
}

stop_streamers() {
	remote_sh "killall venc 2>/dev/null; killall majestic 2>/dev/null; sleep 1; true" || true
}

reboot_target() {
	log "Rebooting ${HOST}..."
	remote_sh "reboot >/dev/null 2>&1 &" || true
	sleep 5
	wait_for_ssh
	# Give the kernel a couple seconds to finish bringing up i2c bus + sensor power
	sleep 3
}

push_regscan_if_needed() {
	if [[ "${SKIP_PUSH}" -eq 1 ]]; then
		log "--skip-push set, assuming regscan is on target."
		return
	fi
	[[ -x "${ROOT_DIR}/out/maruko/regscan" ]] || \
		die "out/maruko/regscan missing — run: make regscan SOC_BUILD=maruko"
	log "Pushing regscan -> /usr/bin/regscan"
	push_stream "${ROOT_DIR}/out/maruko/regscan" "/usr/bin/regscan.new"
	remote_sh "chmod +x /usr/bin/regscan.new && mv /usr/bin/regscan.new /usr/bin/regscan"
}

dump_regs() {
	# dump_regs <i2c_dev> <out_file>
	local dev="$1" out="$2"
	remote_sh "/usr/bin/regscan -d ${dev} -a ${I2C_ADDR} -q" > "${out}"
}

snapshot_and_brightness() {
	# snapshot_and_brightness <port> <path> <out_jpg> <brightness_file>
	#
	# venc exposes /api/v1/snapshot.jpg from v0.10.9 onward (MJPEG VENC
	# channel tapped off the main pipeline).  Majestic uses /snapshot.jpg.
	# Pre-0.10.9 venc returns 404 here; the script records that case as
	# "endpoint_unavailable (HTTP 404)" and the report flags the scenario
	# as eyeball-only.
	local port="$1" path="$2" out_jpg="$3" out_brightness="$4"
	local ip http_code
	ip="$(target_ip)"
	http_code="$(curl -s -m 10 -o "${out_jpg}" -w '%{http_code}' "http://${ip}:${port}${path}" || echo 000)"
	if [[ "${http_code}" != "200" ]]; then
		# Replace whatever error body landed in the jpg slot with nothing.
		rm -f "${out_jpg}"
		echo "endpoint_unavailable (HTTP ${http_code})" > "${out_brightness}"
		return
	fi
	if [[ ! -s "${out_jpg}" ]]; then
		echo "no_snapshot" > "${out_brightness}"
		return
	fi
	# Compute mean luminance.  Prefer ImageMagick, fall back to ffmpeg, else "n/a".
	if command -v identify >/dev/null 2>&1; then
		identify -format 'mean=%[mean]\n' "${out_jpg}" > "${out_brightness}" 2>/dev/null || \
			echo "identify_failed" > "${out_brightness}"
	elif command -v ffmpeg >/dev/null 2>&1; then
		ffmpeg -loglevel error -i "${out_jpg}" -vf signalstats -f null - 2>&1 | \
			grep -oE 'YAVG:[0-9.]+' | head -1 > "${out_brightness}" || \
			echo "ffmpeg_failed" > "${out_brightness}"
	else
		echo "no_tool_installed" > "${out_brightness}"
	fi
}

detect_live_streamer() {
	# Echoes one of: venc | majestic | none
	# Busybox `ps w` lines look like: "  1793 root  4:04 venc" — match the
	# command at end-of-line (or before any whitespace).  Filter out kernel
	# threads in [brackets] (e.g. "[venc0_P0_MAIN]").
	local procs
	procs="$(remote_sh "ps w" 2>/dev/null || true)"
	if grep -E '[[:space:]]venc([[:space:]]|$)' <<< "${procs}" | grep -qv '\['; then
		echo venc
		return
	fi
	if grep -E '[[:space:]]majestic([[:space:]]|$)' <<< "${procs}" | grep -qv '\['; then
		echo majestic
		return
	fi
	echo none
}

run_scenario_E() {
	log "Scenario E — current live state (no reboot)"
	local who
	who="$(detect_live_streamer)"
	log "  live streamer: ${who}"
	echo "${who}" > "${OUT_DIR}/E_current.streamer"
	dump_regs "${I2C_DEV}" "${OUT_DIR}/E_current.regs"
	case "${who}" in
		venc)
			local vport
			vport="$(remote_sh "grep -oE '\"webPort\"[^,}]+' /etc/venc.json | head -1 | grep -oE '[0-9]+' || echo 80")"
			snapshot_and_brightness "${vport}" "/api/v1/snapshot.jpg" \
				"${OUT_DIR}/E_current.jpg" "${OUT_DIR}/E_current.brightness"
			;;
		majestic)
			snapshot_and_brightness "80" "/snapshot.jpg" \
				"${OUT_DIR}/E_current.jpg" "${OUT_DIR}/E_current.brightness"
			;;
		none)
			warn "no streamer running — scenario E is regs only, no snapshot."
			echo "no_streamer" > "${OUT_DIR}/E_current.brightness"
			;;
	esac
	log "  saved ${OUT_DIR}/E_current.*"
}

run_scenario_A() {
	log "Scenario A — firstboot (no streamer)"
	reboot_target
	stop_streamers
	dump_regs "${I2C_DEV}" "${OUT_DIR}/A_firstboot.regs"
}

run_scenario_B() {
	log "Scenario B — venc cold"
	reboot_target
	stop_streamers
	remote_sh "(venc > /tmp/venc.log 2>&1 &); sleep ${SETTLE_SECS}"
	dump_regs "${I2C_DEV}" "${OUT_DIR}/B_venc_cold.regs"
	# venc default webPort 80 — read it from the config to be safe.
	local vport
	vport="$(remote_sh "grep -oE '\"webPort\"[^,}]+' /etc/venc.json | head -1 | grep -oE '[0-9]+' || echo 80")"
	snapshot_and_brightness "${vport}" "/api/v1/snapshot.jpg" \
		"${OUT_DIR}/B_venc_cold.jpg" "${OUT_DIR}/B_venc_cold.brightness"
	remote_sh "killall venc; sleep 1; true" || true
}

run_scenario_C() {
	log "Scenario C — majestic cold"
	reboot_target
	stop_streamers
	remote_sh "(majestic --logfile /tmp/majestic.log >/dev/null 2>&1 &); sleep ${SETTLE_SECS}"
	dump_regs "${I2C_DEV}" "${OUT_DIR}/C_majestic_cold.regs"
	snapshot_and_brightness "80" "/snapshot.jpg" \
		"${OUT_DIR}/C_majestic_cold.jpg" "${OUT_DIR}/C_majestic_cold.brightness"
	remote_sh "killall majestic; sleep 1; true" || true
}

run_scenario_D() {
	log "Scenario D — majestic-then-venc"
	reboot_target
	stop_streamers
	remote_sh "(majestic --logfile /tmp/majestic.log >/dev/null 2>&1 &); sleep ${MAJESTIC_BRIEF_SECS}; killall majestic; sleep 1"
	remote_sh "(venc > /tmp/venc.log 2>&1 &); sleep ${SETTLE_SECS}"
	dump_regs "${I2C_DEV}" "${OUT_DIR}/D_majestic_then_venc.regs"
	local vport
	vport="$(remote_sh "grep -oE '\"webPort\"[^,}]+' /etc/venc.json | head -1 | grep -oE '[0-9]+' || echo 80")"
	snapshot_and_brightness "${vport}" "/api/v1/snapshot.jpg" \
		"${OUT_DIR}/D_majestic_then_venc.jpg" "${OUT_DIR}/D_majestic_then_venc.brightness"
	remote_sh "killall venc; sleep 1; true" || true
}

compute_diff() {
	# compute_diff <left> <right> <out>
	local left="$1" right="$2" out="$3"
	# Filter out header lines and separators, keep only register lines.
	# Then diff side-by-side and emit only differing rows.
	diff -u \
		<(grep -E '^0x[0-9A-F]+' "${left}") \
		<(grep -E '^0x[0-9A-F]+' "${right}") > "${out}" || true
}

regs_only_in_first() {
	# regs_only_in_first <left> <right> <out>
	# Find registers whose value is different between left and right;
	# output: 0xRRRR  left=0xXX  right=0xYY
	local left="$1" right="$2" out="$3"
	awk '
		NR==FNR {
			if ($1 ~ /^0x/) lv[$1]=$3
			next
		}
		$1 ~ /^0x/ {
			if (lv[$1] != "" && lv[$1] != $3) {
				printf "%s  left=%s  right=%s\n", $1, lv[$1], $3
			}
		}
	' "${left}" "${right}" > "${out}"
}

write_report() {
	local report="${OUT_DIR}/report.md"
	{
		echo "# Maruko sensor init diff — ${TIMESTAMP}"
		echo
		echo "- Host:     ${HOST}"
		echo "- i2c dev:  ${I2C_DEV}"
		echo "- i2c addr: ${I2C_ADDR}"
		echo "- Settle:   ${SETTLE_SECS}s"
		echo
		echo "## Snapshot brightness (mean luminance, higher = brighter)"
		echo
		for sc in E_current B_venc_cold C_majestic_cold D_majestic_then_venc; do
			if [[ -s "${OUT_DIR}/${sc}.brightness" ]]; then
				printf -- "- %s: %s\n" "${sc}" "$(tr -d '\n' < "${OUT_DIR}/${sc}.brightness")"
			fi
		done
		echo
		echo "## Registers majestic touches that venc does not (C vs B)"
		echo
		echo "Each line: \`<reg>  venc_value  majestic_value\`"
		echo
		echo '```'
		cat "${OUT_DIR}/diff_C_vs_B.txt"
		echo '```'
		echo
		echo "## venc inheriting from majestic (D vs B)"
		echo
		echo "Registers where running majestic first then venc produces a"
		echo "different sensor state than venc alone — these are the candidates"
		echo "for the fix (venc should write these itself)."
		echo
		echo '```'
		cat "${OUT_DIR}/diff_D_vs_B.txt"
		echo '```'
		echo
		echo "## Cold-state baselines (for context)"
		echo
		echo "### What venc writes from cold (B vs A)"
		echo '```'
		cat "${OUT_DIR}/diff_B_vs_A.txt"
		echo '```'
		echo
		echo "### What majestic writes from cold (C vs A)"
		echo '```'
		cat "${OUT_DIR}/diff_C_vs_A.txt"
		echo '```'
		echo
		echo "## Next steps"
		echo
		echo "1. Look at the 'C vs B' table above — registers in that list"
		echo "   are the smoking gun (majestic sets them, venc does not)."
		echo "2. Confirm with 'D vs B': if venc inherits and the image is now"
		echo "   bright, the registers there are the *minimum* fix surface."
		echo "3. Map the addresses against the IMX415 datasheet to identify"
		echo "   which functional block (gain / exposure / analog gain"
		echo "   trim / black-level) needs the kick."
		echo "4. Add the missing writes to venc's sensor init path (likely"
		echo "   in src/sensor_select.c or via a one-time SDK call before"
		echo "   MI_SNR_SetRes)."
	} > "${report}"
	log "Wrote ${report}"
}

probe_i2c_no_reboot() {
	# Probe i2c-0/1/2 with whatever streamer is already up (or none).
	# Sensor IC keeps state across process exit, so it should still ack.
	if [[ "${I2C_DEV}" != "auto" ]]; then
		echo "${I2C_DEV}"
		return
	fi
	local dev val
	for dev in /dev/i2c-0 /dev/i2c-1 /dev/i2c-2; do
		val="$(remote_sh "/usr/bin/regscan -d ${dev} -a ${I2C_ADDR} -q 2>/dev/null | awk '/^0x3000/ {print \$3; exit}'" || true)"
		if [[ -n "${val}" && "${val}" != "<read" ]]; then
			echo "${dev}"
			return
		fi
	done
	die "could not find a working i2c-dev (tried 0/1/2 at addr ${I2C_ADDR})"
}

main() {
	log "Output: ${OUT_DIR}"
	push_regscan_if_needed

	if [[ "${MODE}" == "current" ]]; then
		# Live-capture path: do NOT reboot anything.  Use whatever is
		# running as the source-of-truth "this is the good state" snapshot.
		I2C_DEV="$(probe_i2c_no_reboot)"
		log "Sensor responds on ${I2C_DEV} @ ${I2C_ADDR}"
		run_scenario_E
		log "Current-only capture complete: ${OUT_DIR}"
		exit 0
	fi

	if [[ "${I2C_DEV}" == "auto" ]]; then
		log "Auto-probing i2c bus..."
		# First ensure regscan is on the box.  Reboot to make sure no streamer
		# is holding the sensor, then probe.
		reboot_target
		stop_streamers
		I2C_DEV="$(probe_i2c)"
		log "Sensor responds on ${I2C_DEV} @ ${I2C_ADDR}"
		# Capture A here while we already have a cold target.
		dump_regs "${I2C_DEV}" "${OUT_DIR}/A_firstboot.regs"
	else
		run_scenario_A
	fi
	run_scenario_B
	run_scenario_C
	run_scenario_D
	compute_diff "${OUT_DIR}/A_firstboot.regs"        "${OUT_DIR}/B_venc_cold.regs"            "${OUT_DIR}/_unified_B_vs_A.diff"
	compute_diff "${OUT_DIR}/A_firstboot.regs"        "${OUT_DIR}/C_majestic_cold.regs"        "${OUT_DIR}/_unified_C_vs_A.diff"
	compute_diff "${OUT_DIR}/B_venc_cold.regs"        "${OUT_DIR}/C_majestic_cold.regs"        "${OUT_DIR}/_unified_C_vs_B.diff"
	compute_diff "${OUT_DIR}/B_venc_cold.regs"        "${OUT_DIR}/D_majestic_then_venc.regs"   "${OUT_DIR}/_unified_D_vs_B.diff"
	regs_only_in_first "${OUT_DIR}/A_firstboot.regs"      "${OUT_DIR}/B_venc_cold.regs"          "${OUT_DIR}/diff_B_vs_A.txt"
	regs_only_in_first "${OUT_DIR}/A_firstboot.regs"      "${OUT_DIR}/C_majestic_cold.regs"      "${OUT_DIR}/diff_C_vs_A.txt"
	regs_only_in_first "${OUT_DIR}/B_venc_cold.regs"      "${OUT_DIR}/C_majestic_cold.regs"      "${OUT_DIR}/diff_C_vs_B.txt"
	regs_only_in_first "${OUT_DIR}/B_venc_cold.regs"      "${OUT_DIR}/D_majestic_then_venc.regs" "${OUT_DIR}/diff_D_vs_B.txt"
	write_report
	log "Done.  Open ${OUT_DIR}/report.md"
}

main "$@"
