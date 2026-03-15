#!/usr/bin/env bash
# audio_verify.sh — Receive and verify audio from venc UDP output
#
# Supports both RTP mode (PT 110) and compact mode (0xAA header).
# Uses GStreamer or ffmpeg depending on what's available.
#
# Usage:
#   ./audio_verify.sh [options]
#
# Options:
#   --port PORT        UDP listen port (default: 5600, or audioPort if set)
#   --codec CODEC      Audio codec: pcm, g711a, g711u, g726 (default: pcm)
#   --rate RATE        Sample rate in Hz (default: 16000)
#   --channels CH      Channel count: 1 or 2 (default: 1)
#   --mode MODE        Stream mode: rtp or compact (default: rtp)
#   --duration SEC     Recording duration in seconds (default: 5)
#   --output FILE      Output file (default: /tmp/venc_audio_test.wav)
#   --play             Play audio through speakers instead of saving
#   --tool TOOL        Force tool: gstreamer or ffmpeg (default: auto-detect)
#   --dump             Hex-dump raw UDP packets (for debugging)
#   -h, --help         Show this help
#
# Examples:
#   # Listen on port 5600 for RTP PCM audio, save 5s to WAV:
#   ./audio_verify.sh --port 5600 --codec pcm --rate 16000
#
#   # Listen on dedicated audio port for G.711 A-law, play live:
#   ./audio_verify.sh --port 5700 --codec g711a --rate 8000 --play
#
#   # Debug: hex-dump first packets on shared video port:
#   ./audio_verify.sh --port 5600 --dump --duration 2
#
set -euo pipefail

PORT=5600
CODEC=pcm
RATE=16000
CHANNELS=1
MODE=rtp
DURATION=5
OUTPUT=/tmp/venc_audio_test.wav
PLAY=0
TOOL=auto
DUMP=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--port)      PORT="$2"; shift 2 ;;
		--codec)     CODEC="$2"; shift 2 ;;
		--rate)      RATE="$2"; shift 2 ;;
		--channels)  CHANNELS="$2"; shift 2 ;;
		--mode)      MODE="$2"; shift 2 ;;
		--duration)  DURATION="$2"; shift 2 ;;
		--output)    OUTPUT="$2"; shift 2 ;;
		--play)      PLAY=1; shift ;;
		--tool)      TOOL="$2"; shift 2 ;;
		--dump)      DUMP=1; shift ;;
		-h|--help)
			sed -n '2,/^$/s/^# \?//p' "$0"
			exit 0 ;;
		*) echo "Unknown option: $1"; exit 1 ;;
	esac
done

# ── Tool detection ───────────────────────────────────────────────────

has_gst() { command -v gst-launch-1.0 >/dev/null 2>&1; }
has_ffmpeg() { command -v ffmpeg >/dev/null 2>&1; }
has_socat() { command -v socat >/dev/null 2>&1; }

if [[ "$TOOL" == "auto" ]]; then
	if has_gst; then
		TOOL=gstreamer
	elif has_ffmpeg; then
		TOOL=ffmpeg
	else
		echo "ERROR: Neither gst-launch-1.0 nor ffmpeg found."
		echo "Install GStreamer: apt install gstreamer1.0-tools gstreamer1.0-plugins-good"
		echo "  or ffmpeg:      apt install ffmpeg"
		exit 1
	fi
fi

echo "=== venc audio verification ==="
echo "  Port:     $PORT"
echo "  Codec:    $CODEC"
echo "  Rate:     $RATE Hz"
echo "  Channels: $CHANNELS"
echo "  Mode:     $MODE"
echo "  Duration: ${DURATION}s"
echo "  Tool:     $TOOL"
if [[ $PLAY -eq 1 ]]; then
	echo "  Output:   [speakers]"
else
	echo "  Output:   $OUTPUT"
fi
echo ""

# ── Hex dump mode ────────────────────────────────────────────────────

if [[ $DUMP -eq 1 ]]; then
	echo "Listening for UDP packets on port $PORT (${DURATION}s)..."
	echo "Tip: First byte 0x80 = RTP, 0xAA = compact audio, other = compact video"
	echo ""
	if has_socat; then
		timeout "$DURATION" socat -x UDP-LISTEN:"$PORT",reuseaddr /dev/null 2>&1 || true
	elif command -v tcpdump >/dev/null 2>&1; then
		timeout "$DURATION" tcpdump -i any -X -c 20 "udp port $PORT" 2>&1 || true
	else
		echo "ERROR: Need socat or tcpdump for --dump mode"
		echo "  apt install socat   OR   apt install tcpdump"
		exit 1
	fi
	exit 0
fi

# ── Map codec to GStreamer/ffmpeg parameters ─────────────────────────

# RTP payload type (all use dynamic PT 110 from our implementation)
RTP_PT=110

# GStreamer caps and depayloader
gst_caps=""
gst_depay=""
gst_decode=""

# ffmpeg SDP parameters
ffmpeg_encoding=""
ffmpeg_fmtp=""

case "$CODEC" in
	pcm)
		# 16-bit signed little-endian PCM
		# RTP clock rate = sample rate
		gst_caps="application/x-rtp,media=audio,clock-rate=${RATE},encoding-name=L16,payload=${RTP_PT},channels=${CHANNELS}"
		gst_depay="rtpL16depay"
		gst_decode="audioconvert"
		ffmpeg_encoding="L16"
		;;
	g711a)
		# G.711 A-law (8-bit, typically 8kHz)
		gst_caps="application/x-rtp,media=audio,clock-rate=${RATE},encoding-name=PCMA,payload=${RTP_PT},channels=${CHANNELS}"
		gst_depay="rtppcmadepay"
		gst_decode="alawdec ! audioconvert"
		ffmpeg_encoding="PCMA"
		;;
	g711u)
		# G.711 μ-law (8-bit, typically 8kHz)
		gst_caps="application/x-rtp,media=audio,clock-rate=${RATE},encoding-name=PCMU,payload=${RTP_PT},channels=${CHANNELS}"
		gst_depay="rtppcmudepay"
		gst_decode="mulawdec ! audioconvert"
		ffmpeg_encoding="PCMU"
		;;
	g726)
		# G.726 ADPCM 32kbps
		gst_caps="application/x-rtp,media=audio,clock-rate=${RATE},encoding-name=G726-32,payload=${RTP_PT},channels=${CHANNELS}"
		gst_depay="rtpg726depay"
		gst_decode="audioconvert"
		ffmpeg_encoding="G726-32"
		;;
	*)
		echo "ERROR: Unknown codec '$CODEC' (expected: pcm, g711a, g711u, g726)"
		exit 1
		;;
esac

# ── Compact mode receiver (custom, not supported by standard tools) ──

if [[ "$MODE" == "compact" ]]; then
	echo "Compact mode: capturing raw UDP packets and stripping 4-byte header..."
	RAW_FILE="/tmp/venc_audio_raw_$$.bin"

	if has_socat; then
		# Capture raw UDP data, then strip headers offline
		timeout "$DURATION" socat -u UDP-LISTEN:"$PORT",reuseaddr STDOUT 2>/dev/null > "$RAW_FILE" || true
	else
		echo "ERROR: Compact mode requires socat for raw UDP capture"
		echo "  apt install socat"
		exit 1
	fi

	RAW_SIZE=$(stat -c%s "$RAW_FILE" 2>/dev/null || echo 0)
	if [[ "$RAW_SIZE" -eq 0 ]]; then
		echo "WARNING: No data received. Check that:"
		echo "  - venc is running with audio.enabled=true"
		echo "  - outgoing.streamMode is 'compact'"
		echo "  - Destination IP points to this machine, port $PORT"
		rm -f "$RAW_FILE"
		exit 1
	fi

	# Strip 4-byte compact headers (0xAA 0x01 len_hi len_lo) using python
	STRIPPED="/tmp/venc_audio_stripped_$$.bin"
	python3 -c "
import sys
data = open('$RAW_FILE', 'rb').read()
out = bytearray()
i = 0
frames = 0
while i < len(data) - 4:
    if data[i] == 0xAA and data[i+1] == 0x01:
        plen = (data[i+2] << 8) | data[i+3]
        if i + 4 + plen <= len(data):
            out.extend(data[i+4:i+4+plen])
            i += 4 + plen
            frames += 1
            continue
    i += 1
open('$STRIPPED', 'wb').write(out)
print(f'Extracted {frames} audio frames, {len(out)} bytes')
" 2>&1

	# Convert raw audio to WAV
	if has_ffmpeg; then
		ACODEC="pcm_s16le"
		case "$CODEC" in
			g711a) ACODEC="alaw" ;;
			g711u) ACODEC="mulaw" ;;
			g726)  ACODEC="g726" ;;
		esac
		ffmpeg -y -f "$ACODEC" -ar "$RATE" -ac "$CHANNELS" \
			-i "$STRIPPED" "$OUTPUT" 2>/dev/null
		echo "Saved to: $OUTPUT"
		echo "Play with: ffplay $OUTPUT"
	else
		cp "$STRIPPED" "${OUTPUT%.wav}.raw"
		echo "Saved raw audio to: ${OUTPUT%.wav}.raw"
		echo "Convert manually: ffmpeg -f s16le -ar $RATE -ac $CHANNELS -i ${OUTPUT%.wav}.raw $OUTPUT"
	fi

	rm -f "$RAW_FILE" "$STRIPPED"
	exit 0
fi

# ── RTP mode — GStreamer pipeline ────────────────────────────────────

if [[ "$TOOL" == "gstreamer" ]]; then
	if [[ $PLAY -eq 1 ]]; then
		SINK="autoaudiosink"
	else
		SINK="wavenc ! filesink location=$OUTPUT"
	fi

	echo "Starting GStreamer RTP receiver..."
	echo "  Waiting for audio on UDP port $PORT..."
	echo "  Press Ctrl+C to stop (or wait ${DURATION}s)"
	echo ""

	# Run with timeout
	set +e
	timeout "$DURATION" gst-launch-1.0 -e \
		udpsrc port="$PORT" caps="$gst_caps" \
		! "$gst_depay" \
		! $gst_decode \
		! audioresample \
		! $SINK \
		2>&1
	GST_RET=$?
	set -e

	if [[ $PLAY -eq 0 ]]; then
		if [[ -f "$OUTPUT" ]]; then
			SIZE=$(stat -c%s "$OUTPUT" 2>/dev/null || echo 0)
			echo ""
			echo "Saved: $OUTPUT ($SIZE bytes)"
			echo "Play:  ffplay $OUTPUT  or  gst-launch-1.0 filesrc location=$OUTPUT ! wavparse ! autoaudiosink"

			# Quick sanity check
			if [[ "$SIZE" -lt 100 ]]; then
				echo ""
				echo "WARNING: Output file is very small ($SIZE bytes)."
				echo "  - Is venc running with audio.enabled=true?"
				echo "  - Is outgoing.streamMode set to 'rtp'?"
				echo "  - Is the destination IP correct?"
			fi
		else
			echo "WARNING: No output file created. No audio data received."
		fi
	fi
	exit 0
fi

# ── RTP mode — ffmpeg pipeline ───────────────────────────────────────

if [[ "$TOOL" == "ffmpeg" ]]; then
	# ffmpeg needs an SDP file to receive RTP
	SDP_FILE="/tmp/venc_audio_$$.sdp"
	cat > "$SDP_FILE" <<-ENDSDP
	v=0
	o=- 0 0 IN IP4 127.0.0.1
	s=venc audio test
	c=IN IP4 127.0.0.1
	t=0 0
	m=audio $PORT RTP/AVP $RTP_PT
	a=rtpmap:$RTP_PT $ffmpeg_encoding/$RATE/$CHANNELS
	ENDSDP
	# Remove leading tabs from heredoc
	sed -i 's/^\t//' "$SDP_FILE"

	if [[ $PLAY -eq 1 ]]; then
		FFOUT="-f pulse default"
		if ! command -v pactl >/dev/null 2>&1; then
			FFOUT="-f alsa default"
		fi
	else
		FFOUT="$OUTPUT"
	fi

	echo "Starting ffmpeg RTP receiver..."
	echo "  SDP: $SDP_FILE"
	echo "  Waiting for audio on UDP port $PORT..."
	echo "  Press Ctrl+C to stop (or wait ${DURATION}s)"
	echo ""

	set +e
	timeout "$DURATION" ffmpeg -y -protocol_whitelist file,udp,rtp \
		-i "$SDP_FILE" \
		-acodec pcm_s16le \
		-t "$DURATION" \
		$FFOUT \
		2>&1
	set -e

	rm -f "$SDP_FILE"

	if [[ $PLAY -eq 0 && -f "$OUTPUT" ]]; then
		SIZE=$(stat -c%s "$OUTPUT" 2>/dev/null || echo 0)
		echo ""
		echo "Saved: $OUTPUT ($SIZE bytes)"
		echo "Play:  ffplay $OUTPUT"
	fi
	exit 0
fi

echo "ERROR: Unknown tool '$TOOL'"
exit 1
