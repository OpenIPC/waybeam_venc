#!/bin/sh
# waybeam_thread_watch.sh -- high-rate per-thread CPU watcher for one process.
#
# Purpose: answer "which thread inside waybeam is burning CPU, and does it
# oscillate?" with enough time resolution to catch short bursts. The companion
# scripts/cpu_profile.sh characterises the whole system; this one drills into a
# single binary.
#
# Two deliberate design choices, both learned the hard way on SSC338Q:
#
#  1. NEVER reads /proc/stat. On this SigmaStar 4.9 kernel the /proc/stat busy
#     fields are flushed in bursts: across identical 2.18 s windows their sum
#     swung 287..605 jiffies while wall time and the per-task counters stayed
#     constant. Busybox `top` divides by that number, which is why it shows
#     waybeam oscillating 2%..25% when the true cost is a steady ~23% of one
#     core. Here every percentage is per-task jiffies over WALL time from
#     /proc/uptime, so the numbers cannot inherit that noise.
#
#  2. Samples fast (default 0.5 s). A 3 s window averages a 0.5 s spike down to
#     nothing, so slow sampling hides exactly the bursts we are hunting.
#
# Percentages are "% of ONE core"; the process total can exceed 100% on the
# dual-core Star6E.
#
# Threads have no pthread_setname_np in waybeam, so all comms read "waybeam".
# They are identified by tid plus the kernel wait channel (wchan) they park in:
#   poll_schedule_timeout  poll/select loop (main GetStream loop)
#   inet_csk_accept        HTTP server accept loop (venc_httpd.c)
#   pipe_wait              stdout log filter (audio_codec.c)
#   hrtimer_nanosleep      periodic timer thread (beacon / ramp / AE)
#   futex_wait_queue_me    worker blocked on a mutex or condvar
#   0                      was ON-CPU at sample time
#
# Usage:
#   ./waybeam_thread_watch.sh                  # 0.5 s samples for 60 s
#   ./waybeam_thread_watch.sh -i 0.25 -n 480   # 0.25 s samples for 120 s
#   ./waybeam_thread_watch.sh -p waybeam-link  # a different process
#   ./waybeam_thread_watch.sh -q               # summary only, no per-sample rows

set -u

INTERVAL=0.5
SAMPLES=120
TARGET=waybeam
QUIET=0

usage() {
	cat <<EOF
Usage: $0 [-i interval] [-n samples] [-p name|pid] [-q]
  -i  seconds per sample, fractional ok  (default $INTERVAL)
  -n  number of samples                  (default $SAMPLES)
  -p  target process name or pid         (default $TARGET)
  -q  summary only, suppress sample rows
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	-i) INTERVAL=$2; shift 2 ;;
	-n) SAMPLES=$2; shift 2 ;;
	-p) TARGET=$2; shift 2 ;;
	-q) QUIET=1; shift ;;
	-h | --help) usage; exit 0 ;;
	*) echo "unknown argument: $1" >&2; usage; exit 1 ;;
	esac
done

case "$TARGET" in
'' | *[!0-9]*) PID=$(pidof "$TARGET" 2>/dev/null | awk '{print $1}') ;;
*) PID=$TARGET ;;
esac
[ -n "${PID:-}" ] && [ -d "/proc/$PID" ] || { echo "target '$TARGET' not running" >&2; exit 1; }

exec awk -v PID="$PID" -v TARGET="$TARGET" -v INTERVAL="$INTERVAL" \
	-v SAMPLES="$SAMPLES" -v QUIET="$QUIET" '
function now(   line, t) {
	close("/proc/uptime")
	getline line < "/proc/uptime"
	close("/proc/uptime")
	split(line, t, " ")
	return t[1] + 0
}

# Busybox awk on OpenIPC is built without math support: no sqrt().
function isqrt(x,   r, i) {
	if (x <= 0) return 0
	r = (x > 1) ? x / 2 : 1
	for (i = 0; i < 40; i++) { if (r <= 0) return 0; r = (r + x / r) / 2 }
	return r
}

# One fork per sample: cat every thread stat in a single glob expansion.
# utime is field 14 and stime field 15, which land at 12 and 13 once the
# "pid (comm) " prefix is stripped.
function snap(   cmd, line, p, q, i, rest, f, n, tid) {
	ntid = 0
	cmd = "cat /proc/" PID "/task/[0-9]*/stat 2>/dev/null"
	while ((cmd | getline line) > 0) {
		p = index(line, " (")
		q = 0
		for (i = length(line); i > 0; i--)
			if (substr(line, i, 1) == ")") { q = i; break }
		if (p == 0 || q == 0) continue
		tid = substr(line, 1, p - 1) + 0
		rest = substr(line, q + 2)
		n = split(rest, f, " ")
		if (n < 13) continue
		cj[tid] = f[12] + f[13]
		ntid++
		if (!(tid in seen)) { seen[tid] = 1; order[++norder] = tid }
	}
	close(cmd)
	t_now = now()
	return ntid
}

# wchan tells us what a thread was doing at sample time; "0" means on-CPU.
function wsnap(   cmd, line, c, path, tid) {
	cmd = "grep . /proc/" PID "/task/[0-9]*/wchan 2>/dev/null"
	while ((cmd | getline line) > 0) {
		c = index(line, ":")
		if (c == 0) continue
		path = substr(line, 1, c - 1)
		sub(/\/wchan$/, "", path)
		sub(/.*\//, "", path)
		wch[path + 0] = substr(line, c + 1)
	}
	close(cmd)
}

function acc(k, v) {
	if (!(k in n_)) { n_[k] = 0; sum[k] = 0; sq[k] = 0; mn[k] = 1e9; mx[k] = -1e9 }
	n_[k]++; sum[k] += v; sq[k] += v * v
	if (v < mn[k]) mn[k] = v
	if (v > mx[k]) mx[k] = v
}
function mean(k) { return n_[k] ? sum[k] / n_[k] : 0 }
function stdev(k,   m, v) {
	if (!n_[k] || n_[k] < 2) return 0
	m = mean(k); v = sq[k] / n_[k] - m * m
	return (v > 0) ? isqrt(v) : 0
}

BEGIN {
	hz = 100

	printf "=== %s (pid %d) per-thread CPU, %s s samples x %d ===\n",
		TARGET, PID, INTERVAL, SAMPLES
	printf "%% of ONE core, from per-task jiffies over wall time.\n"
	printf "/proc/stat is deliberately NOT used -- see header comment.\n\n"

	wsnap()
	snap()
	t0 = t_now
	for (i in cj) pj[i] = cj[i]

	# Stable column order, main thread (tid == pid) first.
	ncol = 0
	col[++ncol] = PID
	for (i = 1; i <= norder; i++) if (order[i] != PID) col[++ncol] = order[i]

	if (!QUIET) {
		printf "%8s %8s", "t", "TOTAL"
		for (i = 1; i <= ncol; i++) printf " %8s", col[i]
		printf "\n"
		printf "%8s %8s", "", ""
		for (i = 1; i <= ncol; i++) printf " %8s", substr(wch[col[i]], 1, 8)
		printf "\n"
	}

	for (s = 1; s <= SAMPLES; s++) {
		system("sleep " INTERVAL)
		p_t = t_now
		if (snap() == 0) {
			printf "\n!! pid %d disappeared at t=%.1f -- process restarted?\n",
				PID, t_now - t0
			break
		}
		wsnap()

		dt = t_now - p_t
		if (dt <= 0) continue
		sc = 100.0 / (dt * hz)

		tot = 0
		for (i = 1; i <= ncol; i++) {
			tid = col[i]
			d = ((tid in cj) && (tid in pj)) ? (cj[tid] - pj[tid]) * sc : 0
			if (d < 0) d = 0
			val[i] = d
			tot += d
			acc("T" tid, d)
		}
		acc("__tot", tot)

		el = t_now - t0
		tser[s] = tot
		telapsed[s] = el
		for (i = 1; i <= ncol; i++) pers[s "," i] = val[i]
		for (i = 1; i <= ncol; i++) perw[s "," i] = wch[col[i]]

		if (!QUIET) {
			printf "%8.2f %8.1f", el, tot
			for (i = 1; i <= ncol; i++) printf " %8.1f", val[i]
			printf "\n"
		}

		for (i in cj) pj[i] = cj[i]
		nsamp = s
	}

	# ------------------------------------------------------------- summary --
	printf "\n--- per-thread summary (%% of one core) ---\n"
	printf "%8s %10s %8s %8s %8s %8s  %s\n",
		"tid", "role", "mean", "min", "max", "stdev", "last wchan"
	for (i = 1; i <= ncol; i++) {
		tid = col[i]
		k = "T" tid
		printf "%8d %10s %8.1f %8.1f %8.1f %8.1f  %s\n",
			tid, (tid == PID ? "main" : "worker"),
			mean(k), mn[k], mx[k], stdev(k), wch[tid]
	}
	k = "__tot"
	printf "%8s %10s %8.1f %8.1f %8.1f %8.1f\n",
		"ALL", "process", mean(k), mn[k], mx[k], stdev(k)

	# Oscillation check: a steady process has range and stdev near zero. A
	# periodic burst shows a large max/mean ratio with a small mean.
	printf "\n--- oscillation check on the process total ---\n"
	m = mean("__tot")
	printf "mean=%.1f  min=%.1f  max=%.1f  range=%.1f  stdev=%.1f",
		m, mn["__tot"], mx["__tot"], mx["__tot"] - mn["__tot"], stdev("__tot")
	if (m > 0) printf "  max/mean=%.2fx", mx["__tot"] / m
	printf "\n"
	if (m <= 0.05)
		printf "VERDICT: idle -- process used no measurable CPU.\n"
	else if (stdev("__tot") / m < 0.15)
		printf "VERDICT: steady. stdev is under 15%% of mean -- no real oscillation.\n"
	else
		printf "VERDICT: genuinely variable. See the outlier samples below.\n"

	# Highest samples, with per-thread attribution, so a burst is traceable to
	# a thread and to what it was waiting on.
	thresh = m + 2 * stdev("__tot")
	printf "\nsamples above mean+2sigma (%.1f):\n", thresh
	nout = 0
	for (s = 1; s <= nsamp; s++) {
		if (tser[s] < thresh) continue
		nout++
		printf "  t=%7.2f total=%6.1f  ", telapsed[s], tser[s]
		for (i = 1; i <= ncol; i++)
			if (pers[s "," i] > 0.5)
				printf "%d=%.1f(%s) ", col[i], pers[s "," i],
					substr(perw[s "," i], 1, 12)
		printf "\n"
		if (nout >= 15) { printf "  ... (truncated at 15)\n"; break }
	}
	if (nout == 0) printf "  none -- the process total never spiked.\n"
}
'
