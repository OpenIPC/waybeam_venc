#!/bin/sh
# cv610_reload_free_verify.sh -- run ON the CV610 target.
#
# Verifies that a venc restart no longer reloads the MPP modules, and that the
# cases which make that unsafe are caught rather than tolerated.
#
# The interesting case is a restart across a MODE CHANGE. A same-mode restart
# passes even when the VB config was silently inherited, because the inherited
# layout happens to be the right one -- so a suite that only restarts in place
# proves nothing about the thing being changed. T3 changes video0.width/height,
# which moves out_blk and the pool count (sys_setup() adds a third pool only
# when out_blk < yuv_blk), and then checks which path sys_setup() actually took.
#
# Instruments are the markers sys_setup() prints:
#   "ok  ss_mpi_vb_set_cfg"                          -> fresh config applied
#   "ok  ss_mpi_vb_set_cfg  (adopted identical ...)" -> BUSY, layout matched
#   "FAIL VB is held by a dead owner ..."            -> BUSY, layout differed
#
# Two steps can kill the SoC, by design, and need an operator who can power
# cycle:
#   T1 attempts the unload the guard is supposed to refuse. If the guard has
#      regressed, the unload proceeds under the hub and wedges the board --
#      that is exactly the bug, observed as no network and ARP incomplete.
#   T5 hard-kills venc so MPP state is left dirty on purpose.
# Everything else is a normal restart.
#
# Usage: cv610_reload_free_verify.sh [--keep-going]
set -u

INIT=/etc/init.d/S95waybeam
HUB=/etc/init.d/S97waybeam-hub
LOADER=/usr/bin/load-cv610-online
CFG=/etc/waybeam.json
LOG=/var/run/waybeam.log
BACKUP=/tmp/waybeam.json.verify-backup
KEEP=0
[ "${1:-}" = "--keep-going" ] && KEEP=1

pass=0; fail=0; note=0
ok()   { pass=$((pass+1)); echo "  PASS  $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL  $1"; [ "$KEEP" = 1 ] || { echo; echo "halting so the state can be inspected"; summary; exit 1; }; }
info() { note=$((note+1)); echo "  NOTE  $1"; }
summary() { echo; echo "=== $pass passed, $fail failed, $note measurement(s) ==="; }
chk()  { desc=$1; shift; if "$@"; then ok "$desc"; else bad "$desc"; fi; }

mods()    { grep -c '^open' /proc/modules; }
venc_pid(){ pidof waybeam 2>/dev/null; }
hub_pid() { pidof waybeam_hub 2>/dev/null; }
pkts()    { wget -qO- --timeout=3 http://127.0.0.1:80/api/v1/transport/status 2>/dev/null \
             | sed 's/.*"packetsSent"://; s/[^0-9].*//'; }

flowing() {
	a=$(pkts); sleep 3; b=$(pkts)
	[ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt "$a" ] 2>/dev/null
}

set_out_size() {
	if command -v json_cli >/dev/null 2>&1; then
		json_cli "$CFG" set video0.width "$1" >/dev/null 2>&1 || return 1
		json_cli "$CFG" set video0.height "$2" >/dev/null 2>&1 || return 1
	else
		sed -i "s/\"width\"[[:space:]]*:[[:space:]]*[0-9]\+/\"width\": $1/;
		        s/\"height\"[[:space:]]*:[[:space:]]*[0-9]\+/\"height\": $2/" "$CFG" || return 1
	fi
	return 0
}

# ---------------------------------------------------------------- preconditions
echo "=== preconditions ==="
cp "$CFG" "$BACKUP" || { echo "cannot back up $CFG"; exit 1; }
echo "  config backed up to $BACKUP"
[ -n "$(venc_pid)" ] || $INIT start >/dev/null 2>&1
[ -n "$(hub_pid)" ]  || $HUB  start >/dev/null 2>&1
sleep 8
base_mods=$(mods)
echo "  venc=$(venc_pid) hub=$(hub_pid) modules=$base_mods"
[ -n "$(hub_pid)" ] || { echo "hub is not running -- T1/T2/T3 need it up"; exit 1; }

# ------------------------------------------------------- T1: the guard still fires
echo
echo "=== T1: unload is refused while the hub holds /dev/rgn ==="
out=$("$LOADER" stop 2>&1); rc=$?
case "$out" in
	*"refusing to unload"*) ok "loader refused, naming holders" ;;
	*) bad "loader did NOT refuse (rc=$rc): $out" ;;
esac
chk "refusal is a non-zero exit" test "$rc" -ne 0
chk "module count unchanged ($base_mods)" test "$(mods)" = "$base_mods"

# ------------------------------------ T2: graceful restart, same mode, hub RUNNING
echo
echo "=== T2: venc restart with the hub up (the case that used to wedge) ==="
: >"$LOG"
$INIT restart >/dev/null 2>&1
sleep 8
chk "venc came back (pid $(venc_pid))" test -n "$(venc_pid)"
chk "hub survived the restart" test -n "$(hub_pid)"
chk "modules never unloaded ($base_mods)" test "$(mods)" = "$base_mods"
chk "frames flowing after restart" flowing
chk "sys_setup ran vb_set_cfg" grep -q "ss_mpi_vb_set_cfg" "$LOG"

# ------------------------------------------- T3: restart ACROSS a mode change
echo
echo "=== T3: restart across a mode change (1080p -> 720p), hub up ==="
if set_out_size 1280 720; then
	: >"$LOG"
	$INIT restart >/dev/null 2>&1
	sleep 8
	if grep -q "adopted identical live config" "$LOG"; then
		bad "VB config was ADOPTED across a mode change -- pools are stale"
	elif grep -q "held by a dead owner" "$LOG"; then
		bad "vb_set_cfg refused after a *graceful* stop -- vb_exit did not release"
	elif grep -q "ok  ss_mpi_vb_set_cfg" "$LOG"; then
		ok "fresh VB config applied for the new layout"
	else
		bad "no vb_set_cfg marker at all"
	fi
	chk "venc running at 720p" test -n "$(venc_pid)"
	chk "frames flowing at 720p" flowing
	chk "still no module reload" test "$(mods)" = "$base_mods"
else
	info "could not edit $CFG -- T3 skipped, mode-change path UNVERIFIED"
fi

# ------------------------------------------------ T4: back to the original mode
echo
echo "=== T4: restore 1080p ==="
cp "$BACKUP" "$CFG"
: >"$LOG"
$INIT restart >/dev/null 2>&1
sleep 8
chk "restored mode: venc up" test -n "$(venc_pid)"
chk "restored mode: streaming" flowing

# ------------------------------------------------------ T5: the crash path
# Expected result is NOT known in advance: if the driver's .release frees VB
# when the process dies, a hard kill self-heals and set_cfg simply succeeds.
# Report what happens rather than asserting.
echo
echo "=== T5: hard-kill venc (no mpp_cleanup), then start in a DIFFERENT mode ==="
echo "    measurement, not an assertion"
killall -9 waybeam 2>/dev/null
sleep 3
set_out_size 1280 720 || info "config edit failed; T5 degraded to a same-mode start"
: >"$LOG"
$INIT start >/dev/null 2>&1
sleep 8
if grep -q "held by a dead owner" "$LOG"; then
	info "VB survived the kill and the mismatch was REFUSED (guard works, reload needed)"
elif grep -q "adopted identical live config" "$LOG"; then
	bad "stale VB adopted across a mode change after a crash -- silent wrong pools"
elif grep -q "ok  ss_mpi_vb_set_cfg" "$LOG"; then
	info "VB was released when the process died -- crash path self-heals"
else
	info "no marker; venc pid=$(venc_pid). Inspect $LOG"
fi

# ------------------------------------------------------------ T6: recovery
echo
echo "=== T6: reload recovery path (hub stopped first, as the guard requires) ==="
cp "$BACKUP" "$CFG"
$HUB stop >/dev/null 2>&1
sleep 2
$INIT reload >/dev/null 2>&1
sleep 10
chk "reload restored venc" test -n "$(venc_pid)"
chk "frames flowing after reload" flowing
$HUB start >/dev/null 2>&1
sleep 5
chk "hub restarted" test -n "$(hub_pid)"

echo
echo "Config restored from $BACKUP."
echo "Still to check by hand: the OSD still renders after T2 (the hub keeps its"
echo "RGN regions now that open_rgn is never unloaded), and one reboot."
summary
[ "$fail" = 0 ]
