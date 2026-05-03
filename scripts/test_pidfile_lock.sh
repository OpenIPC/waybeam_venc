#!/bin/sh
# Verify the venc single-instance pidfile + flock gate (P3 in the May 2026
# code-review bundle).  Runs on a deployed device — second venc launch
# must fail with rc=1 and the "venc already running" banner from
# acquire_pidfile_lock().
#
# Usage:
#   scripts/test_pidfile_lock.sh <host>
#
# Example:
#   scripts/test_pidfile_lock.sh root@192.168.1.13

set -e

HOST="${1:-root@192.168.1.13}"
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

echo "[pidfile] target: $HOST"
echo "[pidfile] expecting an existing venc to be running on the device"

# Verify a venc is already running.  scripts/star6e_direct_deploy.sh cycle
# is the standard way to get one running before this test.
if ! ssh $SSH_OPTS "$HOST" "pidof venc >/dev/null 2>&1"; then
	echo "[pidfile] FAIL: no venc running on $HOST — start one first"
	echo "[pidfile]        e.g. scripts/star6e_direct_deploy.sh cycle"
	exit 1
fi

FIRST_PID=$(ssh $SSH_OPTS "$HOST" "pidof venc | head -1")
echo "[pidfile] first instance pid: $FIRST_PID"

# Try to start a second instance.  It must:
#   - exit non-zero
#   - print the lock-held banner
#   - leave the first instance running
SECOND_OUT=$(ssh $SSH_OPTS "$HOST" "venc 2>&1 || true")
SECOND_RC=$(ssh $SSH_OPTS "$HOST" "venc >/dev/null 2>&1 ; echo \$?")

echo "[pidfile] second-launch rc: $SECOND_RC"
echo "[pidfile] second-launch stdout/stderr:"
echo "$SECOND_OUT" | sed 's/^/  /'

# Verify the original venc is still running.
if ! ssh $SSH_OPTS "$HOST" "kill -0 $FIRST_PID 2>/dev/null"; then
	echo "[pidfile] FAIL: first instance died during second-launch attempt"
	exit 1
fi

# Success criteria.
if [ "$SECOND_RC" != "1" ]; then
	echo "[pidfile] FAIL: expected rc=1 from second launch, got $SECOND_RC"
	exit 1
fi

if ! echo "$SECOND_OUT" | grep -q "venc already running"; then
	echo "[pidfile] FAIL: expected 'venc already running' banner"
	exit 1
fi

# Either banner variant is acceptable; prefer the pidfile-lock-held one.
if echo "$SECOND_OUT" | grep -q "pidfile lock held"; then
	echo "[pidfile] PASS: pidfile + flock gate engaged (primary path)"
else
	echo "[pidfile] PASS: legacy /proc fallback gate engaged"
	echo "[pidfile]       (acceptable, but check why pidfile lock missed)"
fi

exit 0
