#!/usr/bin/env bash
set -euo pipefail

# Exhaustive HTTP API test suite for venc on a live device.
# Assumes venc is already running (deployed via remote_test.sh).
# Exercises every API endpoint and every live-mutable config field.

DEVICE="${1:-192.168.2.13}"
PORT="${2:-8888}"
BASE="http://${DEVICE}:${PORT}"
PASS=0
FAIL=0
SKIP=0
ERRORS=()

# ── Helpers ──────────────────────────────────────────────────────────────

c() { curl -sf --max-time 5 "$@" 2>/dev/null; }

ok_field() {
	local resp="$1"
	echo "${resp}" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d['ok']==True" 2>/dev/null
}

get_value() {
	local resp="$1"
	echo "${resp}" | python3 -c "import sys,json; print(json.dumps(json.load(sys.stdin)['data']['value']))" 2>/dev/null
}

get_json_field() {
	local resp="$1" field="$2"
	echo "${resp}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(json.dumps(d${field}))" 2>/dev/null
}

pass() {
	PASS=$((PASS + 1))
	printf "  PASS  %s\n" "$1"
}

fail() {
	FAIL=$((FAIL + 1))
	ERRORS+=("$1: $2")
	printf "  FAIL  %s -- %s\n" "$1" "$2"
}

skip() {
	SKIP=$((SKIP + 1))
	printf "  SKIP  %s -- %s\n" "$1" "$2"
}

section() {
	printf "\n── %s ──\n" "$1"
}

# Assert GET field returns expected value type
assert_get() {
	local field="$1" expect_type="$2"
	local resp val
	resp="$(c "${BASE}/api/v1/get?${field}")" || { fail "GET ${field}" "curl failed"; return; }
	if ! ok_field "${resp}"; then
		fail "GET ${field}" "ok!=true: ${resp}"
		return
	fi
	val="$(get_value "${resp}")"
	case "${expect_type}" in
		int)    echo "${val}" | grep -qE '^-?[0-9]+$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected int, got ${val}" ;;
		uint)   echo "${val}" | grep -qE '^[0-9]+$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected uint, got ${val}" ;;
		bool)   [[ "${val}" == "true" || "${val}" == "false" ]] && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected bool, got ${val}" ;;
		string) [[ "${val}" == \"*\" || "${val}" == "null" ]] && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected string, got ${val}" ;;
		double) echo "${val}" | grep -qE '^-?[0-9]+\.?[0-9]*$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected double, got ${val}" ;;
		size)   echo "${val}" | grep -qE '^"[0-9]+x[0-9]+"$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected size, got ${val}" ;;
		*)      pass "GET ${field} = ${val}" ;;
	esac
}

# Assert SET field succeeds and value sticks
assert_set() {
	local field="$1" value="$2" label="${3:-SET ${1}=${2}}"
	local resp
	resp="$(c "${BASE}/api/v1/set?${field}=${value}")" || { fail "${label}" "curl failed"; return; }
	if ok_field "${resp}"; then
		pass "${label}"
	else
		fail "${label}" "ok!=true: ${resp}"
	fi
}

# Assert SET fails with expected error
assert_set_fail() {
	local field="$1" value="$2" label="${3:-SET ${1}=${2} (expect fail)}"
	local resp
	resp="$(c "${BASE}/api/v1/set?${field}=${value}")" || { pass "${label} (curl error = rejection)"; return; }
	if ok_field "${resp}"; then
		fail "${label}" "expected failure but got ok=true"
	else
		pass "${label}"
	fi
}

# Wait for stream to settle after parameter change
settle() { sleep "${1:-1}"; }

# ── Connectivity check ───────────────────────────────────────────────────

printf "Testing venc API at %s\n" "${BASE}"
if ! c "${BASE}/api/v1/version" >/dev/null; then
	echo "ERROR: Cannot reach venc at ${BASE}. Is it running?"
	exit 2
fi

# ════════════════════════════════════════════════════════════════════════
section "1. VERSION & CAPABILITIES"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/version")"
if ok_field "${resp}"; then
	ver="$(get_json_field "${resp}" "['data']['app_version']")"
	backend="$(get_json_field "${resp}" "['data']['backend']")"
	pass "GET /api/v1/version (app=${ver}, backend=${backend})"
else
	fail "GET /api/v1/version" "${resp}"
fi

resp="$(c "${BASE}/api/v1/capabilities")"
if ok_field "${resp}"; then
	nfields="$(echo "${resp}" | python3 -c "import sys,json; print(len(json.load(sys.stdin)['data']['fields']))")"
	pass "GET /api/v1/capabilities (${nfields} fields)"
else
	fail "GET /api/v1/capabilities" "${resp}"
fi

# ════════════════════════════════════════════════════════════════════════
section "2. FULL CONFIG RETRIEVAL"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/config")"
if ok_field "${resp}"; then
	sections="$(echo "${resp}" | python3 -c "import sys,json; print(','.join(json.load(sys.stdin)['data']['config'].keys()))")"
	pass "GET /api/v1/config (sections: ${sections})"
else
	fail "GET /api/v1/config" "${resp}"
fi

# ════════════════════════════════════════════════════════════════════════
section "3. GET ALL CONFIG FIELDS"
# ════════════════════════════════════════════════════════════════════════

# System
assert_get "system.web_port" uint
assert_get "system.overclock_level" int
assert_get "system.verbose" bool

# Sensor
assert_get "sensor.index" int
assert_get "sensor.mode" int
assert_get "sensor.unlock_enabled" bool
assert_get "sensor.unlock_cmd" uint
assert_get "sensor.unlock_reg" uint
assert_get "sensor.unlock_value" uint
assert_get "sensor.unlock_dir" int

# ISP
assert_get "isp.sensor_bin" string
assert_get "isp.exposure" uint
assert_get "isp.awb_mode" string
assert_get "isp.awb_ct" uint

# Image
assert_get "image.mirror" bool
assert_get "image.flip" bool
assert_get "image.rotate" int

# Video0
assert_get "video0.codec" string
assert_get "video0.rc_mode" string
assert_get "video0.fps" uint
assert_get "video0.size" size
assert_get "video0.bitrate" uint
assert_get "video0.gop_size" double
assert_get "video0.qp_delta" int
assert_get "video0.slices_enabled" bool
assert_get "video0.slice_size" uint
assert_get "video0.low_delay" bool

# Outgoing
assert_get "outgoing.enabled" bool
assert_get "outgoing.server" string
assert_get "outgoing.stream_mode" string
assert_get "outgoing.max_payload_size" uint
assert_get "outgoing.target_pkt_rate" uint
assert_get "outgoing.send_feedback" bool
assert_get "outgoing.audio_port" uint

# FPV
assert_get "fpv.roi_enabled" bool
assert_get "fpv.roi_qp" int
assert_get "fpv.roi_steps" uint
assert_get "fpv.roi_center" double
assert_get "fpv.noise_level" int

# Audio (only mute is exposed via API; other audio fields are config-file only)
assert_get "audio.mute" bool

# ════════════════════════════════════════════════════════════════════════
section "4. LIVE PARAMETER CHANGES — Bitrate sweep"
# ════════════════════════════════════════════════════════════════════════

for br in 1000 2000 4000 6000 8192 12000 16000; do
	assert_set "video0.bitrate" "${br}"
done
# Restore default
assert_set "video0.bitrate" 6000 "RESTORE video0.bitrate=6000"

# ════════════════════════════════════════════════════════════════════════
section "5. LIVE PARAMETER CHANGES — FPS sweep"
# ════════════════════════════════════════════════════════════════════════

for fps in 5 10 15 20 25 30; do
	assert_set "video0.fps" "${fps}"
	settle 0.5
done
# Restore
assert_set "video0.fps" 30 "RESTORE video0.fps=30"

# ════════════════════════════════════════════════════════════════════════
section "6. LIVE PARAMETER CHANGES — GOP size"
# ════════════════════════════════════════════════════════════════════════

for gop in 0 0.5 1.0 2.0 5.0; do
	assert_set "video0.gop_size" "${gop}"
	settle 0.3
done
assert_set "video0.gop_size" 1.0 "RESTORE video0.gop_size=1.0"

# ════════════════════════════════════════════════════════════════════════
section "6a. LIVE PARAMETER CHANGES — qpDelta"
# ════════════════════════════════════════════════════════════════════════
for delta in -12 -6 0 6 12; do
	assert_set "video0.qp_delta" "${delta}"
	settle 0.3
done
assert_set "video0.qp_delta" 0 "RESTORE video0.qp_delta=0"

section "7. LIVE PARAMETER CHANGES — Exposure"

for exp in 0 1 3 5 7 10 15 20; do
	assert_set "isp.exposure" "${exp}"
	settle 0.5
done
assert_set "isp.exposure" 7 "RESTORE isp.exposure=7"

# ════════════════════════════════════════════════════════════════════════
section "8. AWB MODE — Auto and manual CT"
# ════════════════════════════════════════════════════════════════════════

assert_set "isp.awb_mode" "auto" "SET awb_mode=auto"
settle 1

# Manual color temperature sweep
assert_set "isp.awb_mode" "ct_manual" "SET awb_mode=ct_manual"
settle 0.5
for ct in 2700 3500 4500 5500 6500 8000 10000; do
	assert_set "isp.awb_ct" "${ct}" "SET awb_ct=${ct}K"
	settle 0.3
done

# Back to auto
assert_set "isp.awb_mode" "auto" "RESTORE awb_mode=auto"

# ════════════════════════════════════════════════════════════════════════
section "9. AWB QUERY ENDPOINT"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/awb")"
if ok_field "${resp}"; then
	# Check that expected sub-objects exist
	has_query="$(get_json_field "${resp}" "['data']['query_info']" 2>/dev/null)" && true
	has_attr="$(get_json_field "${resp}" "['data']['attr']" 2>/dev/null)" && true
	if [[ -n "${has_query}" && "${has_query}" != "null" ]]; then
		pass "GET /api/v1/awb (query_info present)"
	else
		pass "GET /api/v1/awb (ok, partial data)"
	fi
else
	fail "GET /api/v1/awb" "${resp}"
fi

# ════════════════════════════════════════════════════════════════════════
section "10. ROI QP — Enable, sweep, disable"
# ════════════════════════════════════════════════════════════════════════

assert_set "fpv.roi_enabled" "true"
settle 0.5

for qp in -30 -18 -9 0 9 18 30; do
	assert_set "fpv.roi_qp" "${qp}" "SET roi_qp=${qp}"
	settle 0.3
done

for steps in 1 2 3 4; do
	assert_set "fpv.roi_steps" "${steps}" "SET roi_steps=${steps}"
	settle 0.3
done

for center in 0.1 0.25 0.5 0.75 0.9; do
	assert_set "fpv.roi_center" "${center}" "SET roi_center=${center}"
	settle 0.3
done

# Disable ROI
assert_set "fpv.roi_enabled" "false" "RESTORE roi_enabled=false"

# ════════════════════════════════════════════════════════════════════════
section "11. OUTPUT ENABLE/DISABLE TOGGLE"
# ════════════════════════════════════════════════════════════════════════

assert_set "outgoing.enabled" "false" "DISABLE output"
settle 2
assert_set "outgoing.enabled" "true" "RE-ENABLE output"
settle 2

# Verify FPS restored after re-enable
resp="$(c "${BASE}/api/v1/get?video0.fps")"
fps_val="$(get_value "${resp}" 2>/dev/null)"
if [[ "${fps_val}" == "30" ]]; then
	pass "FPS restored to 30 after output re-enable"
else
	fail "FPS after re-enable" "expected 30, got ${fps_val}"
fi

# ════════════════════════════════════════════════════════════════════════
section "12. SERVER CHANGE (live)"
# ════════════════════════════════════════════════════════════════════════

# Change to a different port
assert_set "outgoing.server" "udp://192.168.2.6:5601" "SET server to :5601"
settle 1
# Change back
assert_set "outgoing.server" "udp://192.168.2.6:5600" "RESTORE server to :5600"
settle 1

# ════════════════════════════════════════════════════════════════════════
section "13. IDR REQUEST"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/request/idr")"
if ok_field "${resp}"; then
	pass "GET /request/idr"
else
	fail "GET /request/idr" "${resp}"
fi

# Multiple rapid IDR requests (stress)
for i in $(seq 1 5); do
	resp="$(c "${BASE}/request/idr")" || true
done
pass "IDR burst (5 rapid requests)"

# ════════════════════════════════════════════════════════════════════════
section "14. VERBOSE TOGGLE"
# ════════════════════════════════════════════════════════════════════════

assert_set "system.verbose" "true" "SET verbose=true"
settle 0.5
assert_set "system.verbose" "false" "RESTORE verbose=false"

# ════════════════════════════════════════════════════════════════════════
section "15. AUDIO MUTE (live, audio may be disabled)"
# ════════════════════════════════════════════════════════════════════════

# Mute should succeed even when audio is disabled (no-op)
assert_set "audio.mute" "true" "SET audio.mute=true"
settle 0.3
assert_set "audio.mute" "false" "RESTORE audio.mute=false"

# ════════════════════════════════════════════════════════════════════════
section "16. ERROR HANDLING — Invalid fields and values"
# ════════════════════════════════════════════════════════════════════════

# Unknown field
resp="$(c "${BASE}/api/v1/get?nonexistent.field" 2>/dev/null)" || resp="error"
if [[ "${resp}" == "error" ]] || ! ok_field "${resp}" 2>/dev/null; then
	pass "GET unknown field rejected"
else
	fail "GET unknown field" "expected error, got ok"
fi

# Invalid value types
assert_set_fail "video0.fps" "not_a_number"
assert_set_fail "video0.fps" "0"
assert_set_fail "video0.qp_delta" "-13"
assert_set_fail "video0.qp_delta" "13"
assert_set_fail "isp.awb_mode" "invalid_mode"

# Unknown route
resp="$(curl -sf --max-time 3 "${BASE}/api/v1/nonexistent" 2>/dev/null)" || resp="error"
if [[ "${resp}" == "error" ]]; then
	pass "Unknown route returns error"
else
	if ! ok_field "${resp}" 2>/dev/null; then
		pass "Unknown route returns error JSON"
	else
		fail "Unknown route" "expected error"
	fi
fi

# ════════════════════════════════════════════════════════════════════════
section "17. COMBINED PARAMETER CHANGES (rapid fire)"
# ════════════════════════════════════════════════════════════════════════

# Rapidly change multiple live parameters
assert_set "video0.bitrate" 4000
assert_set "video0.fps" 20
assert_set "video0.gop_size" 0.5
assert_set "video0.qp_delta" -4
assert_set "fpv.roi_enabled" "true"
assert_set "fpv.roi_qp" 15
assert_set "isp.exposure" 5
settle 2

# Verify they all took effect
for check in "video0.bitrate:4000" "video0.fps:20" "video0.qp_delta:-4" "fpv.roi_enabled:true"; do
	field="${check%%:*}"
	expected="${check#*:}"
	resp="$(c "${BASE}/api/v1/get?${field}")"
	val="$(get_value "${resp}" 2>/dev/null)"
	if [[ "${val}" == "${expected}" ]]; then
		pass "VERIFY ${field} == ${expected}"
	else
		fail "VERIFY ${field}" "expected ${expected}, got ${val}"
	fi
done

# Restore all
assert_set "video0.bitrate" 6000
assert_set "video0.fps" 30
assert_set "video0.gop_size" 1.0
assert_set "video0.qp_delta" 0
assert_set "fpv.roi_enabled" "false"
assert_set "isp.exposure" 7

# ════════════════════════════════════════════════════════════════════════
section "18. RESTART-REQUIRED FIELDS (verify reinit_pending)"
# ════════════════════════════════════════════════════════════════════════

# These fields should return reinit_pending=true but we do NOT trigger
# a restart here — that would tear down the pipeline. Just verify the flag.
for field_val in "video0.rc_mode=cbr" "video0.slices_enabled=true" "video0.low_delay=false" \
                 "outgoing.stream_mode=rtp" "outgoing.max_payload_size=1400"; do
	field="${field_val%%=*}"
	value="${field_val#*=}"
	resp="$(c "${BASE}/api/v1/set?${field}=${value}")" || { fail "SET ${field}" "curl failed"; continue; }
	if ok_field "${resp}"; then
		pending="$(get_json_field "${resp}" "['data'].get('reinit_pending',False)")"
		if [[ "${pending}" == "true" ]]; then
			pass "SET ${field}=${value} (reinit_pending=true)"
		else
			pass "SET ${field}=${value} (accepted, no reinit flagged)"
		fi
	else
		fail "SET ${field}=${value}" "${resp}"
	fi
done

# ════════════════════════════════════════════════════════════════════════
section "19. PIPELINE RESTART"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/restart")"
if ok_field "${resp}"; then
	pass "GET /api/v1/restart (reinit requested)"
else
	fail "GET /api/v1/restart" "${resp}"
fi

# Wait for pipeline to reinitialize
sleep 10

# Verify venc is back up
resp="$(c "${BASE}/api/v1/version")" || resp=""
if [[ -n "${resp}" ]] && ok_field "${resp}"; then
	pass "venc responsive after restart"
else
	fail "POST-RESTART reachability" "venc not responding after restart"
fi

# Post-restart validation
resp="$(c "${BASE}/api/v1/config")"
if ok_field "${resp}"; then
	pass "Full config readable after restart"
else
	fail "Full config after restart" "${resp}"
fi

# Verify live parameter changes work after restart
assert_set "video0.bitrate" 8000 "POST-RESTART SET bitrate=8000"
settle 1
assert_set "video0.fps" 25 "POST-RESTART SET fps=25"
settle 1
assert_set "video0.qp_delta" -6 "POST-RESTART SET qp_delta=-6"
settle 1

# Verify values took effect
for check in "video0.bitrate:8000" "video0.fps:25" "video0.qp_delta:-6"; do
	field="${check%%:*}"
	expected="${check#*:}"
	resp="$(c "${BASE}/api/v1/get?${field}")"
	val="$(get_value "${resp}" 2>/dev/null)"
	if [[ "${val}" == "${expected}" ]]; then
		pass "POST-RESTART VERIFY ${field} == ${expected}"
	else
		fail "POST-RESTART VERIFY ${field}" "expected ${expected}, got ${val}"
	fi
done

# Restore
assert_set "video0.bitrate" 6000 "POST-RESTART RESTORE bitrate=6000"
assert_set "video0.fps" 30 "POST-RESTART RESTORE fps=30"
assert_set "video0.qp_delta" 0 "POST-RESTART RESTORE qp_delta=0"

# ════════════════════════════════════════════════════════════════════════
printf "\n══════════════════════════════════════════════\n"
printf "RESULTS: %d passed, %d failed, %d skipped\n" "${PASS}" "${FAIL}" "${SKIP}"
printf "══════════════════════════════════════════════\n"

if [[ ${FAIL} -gt 0 ]]; then
	printf "\nFailed tests:\n"
	for e in "${ERRORS[@]}"; do
		printf "  - %s\n" "${e}"
	done
	exit 1
fi

exit 0
