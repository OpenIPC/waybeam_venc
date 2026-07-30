#!/bin/sh
# qr_watch.sh — poll the waybeam JPEG snapshot and decode QR codes.
#
# This standalone helper has no boot window or action dispatch. It grabs
# GET /api/v1/snapshot.jpg and runs qr_decode over it. The next capture
# starts as soon as the previous capture/decode returns, subject to a
# 0.5-second minimum between capture starts. Valid minimal envelopes go to
# stdout and every status line goes to stderr, so it is safe to capture:
#
#   msg="$(qr_watch.sh)"                 # one-shot: exits on the first decode
#   qr_watch.sh -c                       # continuous: keeps scanning, streams
#
# Use one-shot to read a single code; use continuous to leave a scanner running
# while you present codes, tune focus, or check how reliably one decodes.
#
# Capture resolution follows the snapshot.width/height config (default: the
# main stream) — QR range scales with pixels per module, so size the MJPEG
# channel up if codes must decode from further away.
#
# Exit: 0 payload decoded (on stdout) | 1 no decode within the try limit
#       2 setup or usage error.  Continuous mode exits 0 if it decoded at least
#       once, and only stops on the try limit or ctrl-c.
#
# Env overrides (flags win): QR_INTERVAL_S QR_MAX_TRIES QR_ENDPOINT
#                            QR_CONTINUOUS QR_STATS QR_DECODE_BIN QR_TMP_IMG

set -u

INTERVAL_S="${QR_INTERVAL_S:-0.5}"
MIN_INTERVAL_CS=50
MAX_TRIES="${QR_MAX_TRIES:-0}"          # 0 = no limit
ENDPOINT="${QR_ENDPOINT:-http://127.0.0.1/api/v1/snapshot.jpg}"
CONTINUOUS="${QR_CONTINUOUS:-0}"        # 1 = keep scanning past the first hit
STATS="${QR_STATS:-0}"                  # 1 = show qr_decode stage diagnostics
TMP_IMG="${QR_TMP_IMG:-}"
TMP_OWNED=0

log() { echo "[qr-watch] $*" >&2; }
cleanup() {
	if [ "$TMP_OWNED" = "1" ]; then
		rm -f "$TMP_IMG"
	fi
}

if [ -n "${QR_DECODE_BIN:-}" ]; then
	QR_DECODE=$QR_DECODE_BIN
else
	case "$0" in
	*/*) script_dir=${0%/*} ;;
	*) script_dir=. ;;
	esac
	if [ -x "$script_dir/qr_decode" ]; then
		QR_DECODE="$script_dir/qr_decode"
	else
		QR_DECODE=/usr/bin/qr_decode
	fi
fi

interval_to_cs() {
	value="$1"
	case "$value" in
	''|.|*[!0-9.]*|*.*.*)
		return 1
		;;
	esac

	case "$value" in
	*.*)
		whole=${value%%.*}
		fraction=${value#*.}
		;;
	*)
		whole=$value
		fraction=
		;;
	esac
	[ -n "$whole" ] || whole=0
	case "$fraction" in
	'') fraction=0 ;;
	[0-9]) fraction="${fraction}0" ;;
	[0-9][0-9]) ;;
	*) return 1 ;;
	esac

	# Avoid implementations which treat a leading zero as an octal prefix.
	while [ "${whole#0}" != "$whole" ]; do whole=${whole#0}; done
	while [ "${fraction#0}" != "$fraction" ]; do fraction=${fraction#0}; done
	[ -n "$whole" ] || whole=0
	[ -n "$fraction" ] || fraction=0
	INTERVAL_CS=$((whole * 100 + fraction))
	[ "$INTERVAL_CS" -ge "$MIN_INTERVAL_CS" ] ||
		INTERVAL_CS=$MIN_INTERVAL_CS
	display_whole=$((INTERVAL_CS / 100))
	display_fraction=$((INTERVAL_CS % 100))
	[ "$display_fraction" -ge 10 ] ||
		display_fraction="0${display_fraction}"
	INTERVAL_DISPLAY="${display_whole}.${display_fraction}"
}

monotonic_cs() {
	# Linux /proc/uptime is monotonic and exposes two fractional digits.
	# Keep this pure shell so stress polling does not fork a timing utility.
	IFS=' ' read -r uptime_value _ < /proc/uptime || return 1
	uptime_whole=${uptime_value%%.*}
	uptime_fraction=${uptime_value#*.}
	while [ "${uptime_fraction#0}" != "$uptime_fraction" ]; do
		uptime_fraction=${uptime_fraction#0}
	done
	[ -n "$uptime_fraction" ] || uptime_fraction=0
	NOW_CS=$((uptime_whole * 100 + uptime_fraction))
}

sleep_cs() {
	sleep_whole=$(( $1 / 100 ))
	sleep_fraction=$(( $1 % 100 ))
	[ "$sleep_fraction" -ge 10 ] ||
		sleep_fraction="0${sleep_fraction}"
	sleep "${sleep_whole}.${sleep_fraction}"
}

usage() {
	cat >&2 <<EOF
usage: qr_watch.sh [-cv] [-i SECONDS] [-n TRIES] [-e URL]

  -c  continuous — keep scanning and stream every decode, instead of
      exiting on the first one (ctrl-c to stop)
  -v  show bounded-frame stage timings and decode statistics
  -i  minimum seconds between snapshot starts (default $INTERVAL_S,
      values below 0.5 are clamped to 0.5)
  -n  stop after this many snapshots (default ${MAX_TRIES}, 0 = no limit)
  -e  snapshot endpoint (default $ENDPOINT)

Payloads go to stdout, status to stderr.
EOF
}

while getopts ':cvi:n:e:h' opt; do
	case "$opt" in
	c) CONTINUOUS=1 ;;
	v) STATS=1 ;;
	i) INTERVAL_S="$OPTARG" ;;
	n) MAX_TRIES="$OPTARG" ;;
	e) ENDPOINT="$OPTARG" ;;
	h) usage; exit 0 ;;
	:) log "option -$OPTARG needs a value"; usage; exit 2 ;;
	?) log "unknown option -$OPTARG"; usage; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || { log "unexpected argument: $1"; usage; exit 2; }
[ "$STATS" = "0" ] || [ "$STATS" = "1" ] ||
	{ log "QR_STATS must be 0 or 1 (got '$STATS')"; exit 2; }

if [ -z "$TMP_IMG" ]; then
	TMP_IMG="$(mktemp /tmp/qr_watch.XXXXXX)" ||
		{ log "cannot create temporary snapshot file"; exit 2; }
	TMP_OWNED=1
fi
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Numeric gates, so a typo fails now instead of inside the arithmetic below.
interval_to_cs "$INTERVAL_S" ||
	{ log "-i takes non-negative seconds with at most two decimals (got '$INTERVAL_S')"; exit 2; }
case "$MAX_TRIES" in
''|*[!0-9]*) log "-n takes a non-negative integer (got '$MAX_TRIES')"; exit 2 ;;
esac
monotonic_cs ||
	{ log "/proc/uptime is required for monotonic sub-second cadence"; exit 2; }

[ -x "$QR_DECODE" ] || { log "decoder $QR_DECODE not found/executable"; exit 2; }
log "decoder $QR_DECODE"

if [ "$CONTINUOUS" = "1" ]; then
	log "continuous scan of $ENDPOINT, minimum ${INTERVAL_DISPLAY}s between starts (ctrl-c to stop)"
else
	log "polling $ENDPOINT, minimum ${INTERVAL_DISPLAY}s between starts (ctrl-c to stop)"
fi

try=0
hits=0
while :; do
	monotonic_cs
	cycle_started_cs=$NOW_CS
	try=$((try + 1))

	# Keep the body on an HTTP error so the reason is visible rather than
	# looking like an empty frame: 503 = snapshot disabled.
	code="$(curl -sS -m 5 -o "$TMP_IMG" -w '%{http_code}' "$ENDPOINT" 2>/dev/null)"

	if [ "$code" = "200" ]; then
		if [ "$STATS" = "1" ]; then
			# Keep the image first: current decoders accept options in either
			# order, while the legacy decoder treats argv[1] as the filename.
			payload="$("$QR_DECODE" "$TMP_IMG" --stats)"
		else
			payload="$("$QR_DECODE" "$TMP_IMG")"
		fi
		decode_rc=$?
		if [ "$decode_rc" -eq 0 ] && [ -n "$payload" ]; then
			hits=$((hits + 1))
			log "decoded on try $try (hit $hits)"
			printf '%s\n' "$payload"
			if [ "$CONTINUOUS" != "1" ]; then
				exit 0
			fi
		elif [ "$decode_rc" -eq 1 ]; then
			log "try $try: snapshot ok, no QR in frame"
		else
			log "try $try: decoder failed (exit $decode_rc)"
			exit 2
		fi
	elif [ -n "$code" ] && [ "$code" != "000" ]; then
		log "try $try: HTTP $code — $(head -c 200 "$TMP_IMG" 2>/dev/null)"
	else
		log "try $try: no response from $ENDPOINT"
	fi

	if [ "$MAX_TRIES" -gt 0 ] && [ "$try" -ge "$MAX_TRIES" ]; then
		if [ "$hits" -gt 0 ]; then
			log "stopping after $try tries ($hits decoded)"
			exit 0
		fi
		log "giving up after $try tries"
		exit 1
	fi

	# Schedule from the capture start, not from decode completion. A slow
	# capture/decode therefore starts the next sample immediately; a fast one
	# sleeps only the remainder needed to keep starts at least 0.5 s apart.
	monotonic_cs
	cycle_elapsed_cs=$((NOW_CS - cycle_started_cs))
	cycle_remaining_cs=$((INTERVAL_CS - cycle_elapsed_cs))
	if [ "$cycle_remaining_cs" -gt 0 ]; then
		sleep_cs "$cycle_remaining_cs"
	fi
done
