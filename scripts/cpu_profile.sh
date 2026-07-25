#!/bin/sh
# cpu_profile.sh -- time-series CPU profiler for embedded waybeam targets.
#
# A single-shot CPU sample cannot explain *variance*. This samples /proc
# repeatedly and reports mean/min/max/stdev per process, so the biggest mover
# is obvious. It also splits usr vs sys and shows the IRQ/softirq remainder
# that no userspace row accounts for -- on SigmaStar that remainder is the MI
# SDK kernel pipeline (vpe/vif/venc/isp `_P0_MAIN` workers), which is where
# most of the cost lives and is invisible to per-thread profiling.
#
# All percentages are "% of ONE core" (top Irix style), so a 2-core box
# saturates at 200%. Process rows and the system row are directly comparable,
# and per-core columns expose pinning (waybeam runs on cpu0 on Star6E).
#
# LOAD AVERAGE IS USELESS HERE: the MI SDK kernel threads park in
# uninterruptible D-state, which counts toward loadavg at ~0 CPU. A Star6E
# vehicle idles at loadavg ~14. Use the busy% below, never loadavg.
#
# Pure busybox ash + busybox awk. No bash, no procps, no perf, no /proc/config.
#
# Usage:
#   ./cpu_profile.sh                     # 20 x 2s, target waybeam
#   ./cpu_profile.sh -n 60 -i 1 -t       # 60 x 1s with per-thread breakdown
#   ./cpu_profile.sh -p waybeam-link     # profile a different process
#   ./cpu_profile.sh -s                  # add fixed-work spin timing

set -u

SAMPLES=20
INTERVAL=2
TARGET=waybeam
THREADS=0
SPIN=0
DEBUG=0
TOPN=12

usage() {
	cat <<EOF
Usage: $0 [-n samples] [-i interval] [-p name|pid] [-t] [-s] [-T topN]
  -n  number of samples            (default $SAMPLES)
  -i  seconds per sample           (default $INTERVAL)
  -p  target process name or pid   (default $TARGET)
  -t  also break down the target's threads
  -s  time a fixed-work spin each sample (detects clock/contention changes)
  -T  rows in the variance tables  (default $TOPN)
  -d  print raw jiffie counters per sample (accounting debug)
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	-n) SAMPLES=$2; shift 2 ;;
	-i) INTERVAL=$2; shift 2 ;;
	-p) TARGET=$2; shift 2 ;;
	-T) TOPN=$2; shift 2 ;;
	-t) THREADS=1; shift ;;
	-s) SPIN=1; shift ;;
	-d) DEBUG=1; shift ;;
	-h | --help) usage; exit 0 ;;
	*) echo "unknown argument: $1" >&2; usage; exit 1 ;;
	esac
done

# Resolve the target to a pid. A non-numeric target goes through pidof; if it
# is not running we still profile the system (pid 0 = "no target row").
case "$TARGET" in
'' | *[!0-9]*) PID=$(pidof "$TARGET" 2>/dev/null | awk '{print $1}') ;;
*) PID=$TARGET; TARGET=$(awk '{print $2}' "/proc/$TARGET/stat" 2>/dev/null | tr -d '()') ;;
esac
[ -n "${PID:-}" ] || PID=0
[ "$PID" != 0 ] && [ ! -d "/proc/$PID" ] && { echo "pid $PID not running" >&2; exit 1; }
[ "$PID" = 0 ] && echo "note: target '$TARGET' not running -- system-only profile" >&2

NCORES=$(grep -c '^processor' /proc/cpuinfo)
FREQ_F=/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "n/a")
[ -r "$FREQ_F" ] || FREQ_F=""

exec awk -v SAMPLES="$SAMPLES" -v INTERVAL="$INTERVAL" -v PID="$PID" \
	-v TARGET="$TARGET" -v NCORES="$NCORES" -v THREADS="$THREADS" \
	-v SPIN="$SPIN" -v DEBUG="$DEBUG" -v TOPN="$TOPN" -v FREQ_F="$FREQ_F" -v GOV="$GOV" '
# ---------------------------------------------------------------- helpers ----
function now(   line, t) {
	close("/proc/uptime")
	getline line < "/proc/uptime"
	close("/proc/uptime")
	split(line, t, " ")
	return t[1] + 0
}

function curfreq(   line) {
	if (FREQ_F == "") return 0
	close(FREQ_F)
	line = ""
	getline line < FREQ_F
	close(FREQ_F)
	return line + 0
}

# /proc/<pid>/stat: the comm field is parenthesised and may contain spaces, so
# split on the first " (" and the LAST ")". After the comm, field 1 is state,
# which puts utime at 12 and stime at 13.
function statjiffies(line,   p, q, i, rest, f, n) {
	p = index(line, " (")
	q = 0
	for (i = length(line); i > 0; i--)
		if (substr(line, i, 1) == ")") { q = i; break }
	if (p == 0 || q == 0) return -1
	_pid = substr(line, 1, p - 1) + 0
	_comm = substr(line, p + 2, q - p - 2)
	rest = substr(line, q + 2)
	n = split(rest, f, " ")
	if (n < 13) return -1
	_ut = f[12]; _st = f[13]
	return _ut + _st
}

# One pass over /proc. A single `cat` of the expanded glob is one fork per
# sample -- far cheaper than a shell loop, which matters at 1.2 GHz.
function snap(   line, f, cmd, j, c, i) {
	t_now = now()
	f_now = curfreq()

	close("/proc/stat")
	while ((getline line < "/proc/stat") > 0) {
		split(line, f, " ")
		if (f[1] == "cpu") {
			s_usr = f[2] + f[3]; s_sys = f[4]
			s_idle = f[5] + f[6]; s_irq = f[7]; s_sirq = f[8]
			s_tot = f[2]+f[3]+f[4]+f[5]+f[6]+f[7]+f[8]+f[9]
		} else if (substr(f[1], 1, 3) == "cpu") {
			c = substr(f[1], 4) + 0
			core_idle[c] = f[5] + f[6]
		} else if (f[1] == "ctxt") {
			s_ctxt = f[2]
		} else if (f[1] == "intr") {
			s_intr = f[2]
		}
	}
	close("/proc/stat")

	for (i in cj) delete cj[i]
	cmd = "cat /proc/[0-9]*/stat 2>/dev/null"
	while ((cmd | getline line) > 0) {
		j = statjiffies(line)
		if (j < 0) continue
		cj[_pid] = j
		cu[_pid] = _ut
		cs[_pid] = _st
		cname[_pid] = _comm
	}
	close(cmd)

	if (THREADS && PID > 0) {
		for (i in tj) delete tj[i]
		cmd = "cat /proc/" PID "/task/[0-9]*/stat 2>/dev/null"
		while ((cmd | getline line) > 0) {
			j = statjiffies(line)
			if (j < 0) continue
			tj[_pid] = j
			tname[_pid] = _comm
		}
		close(cmd)
	}
}

# A fixed amount of integer work. Its wall time is flat if the core clock and
# our share of it are flat, and stretches under contention or throttling.
#
# Keep the iteration count LOW: interpreted awk arithmetic costs ~8 us/iter on a
# 1.2 GHz ARMv7, so this probe is itself a real load. 20k iters is ~150 ms and
# still perturbs the sample it runs in -- that is why -s is opt-in. Use it to
# answer "is the clock stable", then re-run without it to read real numbers.
function spin(   i, x, t0) {
	t0 = now()
	x = 0
	for (i = 0; i < 20000; i++) x = x + i % 7
	return (now() - t0) * 1000.0
}

function acc(key, val) {
	if (!(key in n_)) { n_[key] = 0; sum[key] = 0; sq[key] = 0; mn[key] = 1e9; mx[key] = -1e9 }
	n_[key]++
	sum[key] += val
	sq[key] += val * val
	if (val < mn[key]) mn[key] = val
	if (val > mx[key]) mx[key] = val
}

function mean(key) { return n_[key] ? sum[key] / n_[key] : 0 }

# Busybox awk on OpenIPC is built without math support, so sqrt() is missing.
# Newton-Raphson converges in far fewer than 40 steps for our magnitudes.
function isqrt(x,   r, i) {
	if (x <= 0) return 0
	r = (x > 1) ? x / 2 : 1
	for (i = 0; i < 40; i++) {
		if (r <= 0) return 0
		r = (r + x / r) / 2
	}
	return r
}

function stdev(key,   m, v) {
	if (!n_[key] || n_[key] < 2) return 0
	m = mean(key)
	v = sq[key] / n_[key] - m * m
	return (v > 0) ? isqrt(v) : 0
}

# Descending selection sort over keys[1..cnt] by metric[] -- small N, and
# busybox awk has no asort.
function sortkeys(cnt, metric, keys,   i, k, best, tmp) {
	for (i = 1; i < cnt; i++) {
		best = i
		for (k = i + 1; k <= cnt; k++)
			if (metric[keys[k]] > metric[keys[best]]) best = k
		if (best != i) { tmp = keys[i]; keys[i] = keys[best]; keys[best] = tmp }
	}
}

# ------------------------------------------------------------------- main ----
BEGIN {
	ncores = NCORES + 0
	if (ncores < 1) ncores = 1
	hz = 100          # USER_HZ; verified against measured jiffies below

	"hostname" | getline host
	close("hostname")

	printf "=== waybeam CPU variance profile -- %s ===\n", host
	printf "cores=%d  governor=%s  freq=%s kHz  target=%s pid=%s\n",
		ncores, GOV, (curfreq() ? curfreq() : "n/a"), TARGET, (PID ? PID : "-")
	printf "%d samples x %ss   |   all figures are %% of ONE core (max %d%%)\n",
		SAMPLES, INTERVAL, ncores * 100
	printf "loadavg is meaningless on SigmaStar (D-state SDK threads) -- ignore it\n\n"

	snap()
	t_start = t_now
	p_t = t_now; p_usr = s_usr; p_sys = s_sys; p_idle = s_idle
	p_irq = s_irq; p_sirq = s_sirq; p_tot = s_tot
	p_ctxt = s_ctxt; p_intr = s_intr
	for (i in cj) { pj[i] = cj[i]; pu[i] = cu[i]; ps_[i] = cs[i] }
	for (i in tj) ptj[i] = tj[i]
	for (c in core_idle) pci[c] = core_idle[c]

	printf "     t   busy    usr    sys   sirq"
	for (c = 0; c < ncores; c++) printf "    cpu%d", c
	printf " %8s  unattr   ctxsw/s", substr(TARGET, 1, 8)
	if (SPIN) printf "  spin_ms"
	printf "\n"

	for (s = 1; s <= SAMPLES; s++) {
		system("sleep " INTERVAL)
		spin_ms = SPIN ? spin() : 0
		snap()

		dt = t_now - p_t
		if (dt <= 0) dt = INTERVAL + 0

		# `one` = jiffies of wall time one core can offer in this window; `avail`
		# = the same across all cores. Everything is scaled to "% of one core"
		# by sc, so process rows and system rows are directly comparable.
		one = dt * hz
		avail = one * ncores
		sc = 100.0 / one

		# Derive busy from IDLE, not from the sum of the busy fields. On this
		# SigmaStar 4.9 kernel the busy fields (notably `system`) are flushed in
		# bursts, so over a 2 s window their sum swings 287..561 jiffies while
		# `idle` and the per-task counters stay stable. Trusting the field sum
		# put UNATTRIBUTED at -96%; deriving from idle keeps it near zero.
		dtot = s_tot - p_tot
		didle = s_idle - p_idle
		if (didle > avail) didle = avail
		if (didle < 0) didle = 0
		busy = (avail - didle) * sc

		# The kernel usr/sys fields share the same burst-flush problem, so the
		# usr/sys split below is summed from the per-task counters instead (see
		# the loop). Kept here only for -d comparison.
		k_usr = (s_usr - p_usr) * sc
		k_sys = (s_sys - p_sys) * sc
		sirq = ((s_sirq - p_sirq) + (s_irq - p_irq)) * sc
		ctxsw = (s_ctxt - p_ctxt) / dt

		# Per-process deltas; sum them so we can size the unattributed remainder.
		procsum = 0
		procj = 0
		npid = 0
		tgt = 0
		usr = 0
		sys = 0
		for (i in cj) {
			npid++
			if (!(i in pj)) continue
			dj = cj[i] - pj[i]
			if (dj <= 0) continue
			d = dj * sc
			usr += (cu[i] - pu[i]) * sc
			sys += (cs[i] - ps_[i]) * sc
			procj += dj
			procsum += d
			acc("P" i, d)
			pnm["P" i] = cname[i]
			if (i == PID) tgt = d
		}
		acc("__busy", busy); acc("__usr", usr); acc("__sys", sys)
		acc("__sirq", sirq); acc("__tgt", tgt)
		acc("__unattr", busy - procsum)
		acc("__ctxsw", ctxsw)
		if (SPIN) acc("__spin", spin_ms)
		if (f_now) acc("__freq", f_now)

		printf "%6.1f %6.1f %6.1f %6.1f %6.1f", t_now - t_start, busy, usr, sys, sirq
		for (c = 0; c < ncores; c++) {
			ci = core_idle[c] - pci[c]
			if (ci > one) ci = one
			if (ci < 0) ci = 0
			cpct = (one > 0) ? (one - ci) * 100.0 / one : 0
			printf " %7.1f", cpct
			acc("__core" c, cpct)
		}
		printf " %8.1f %7.1f %9.0f", tgt, busy - procsum, ctxsw
		if (SPIN) printf " %8.1f", spin_ms
		if (DEBUG)
			printf "   [dt=%.2f dtot=%d didle=%d dbusy=%d dprocj=%d npid=%d]",
				dt, dtot, s_idle - p_idle, dtot - (s_idle - p_idle), procj, npid
		printf "\n"

		if (THREADS) {
			for (i in tj) {
				if (!(i in ptj)) continue
				d = (tj[i] - ptj[i]) * sc
				acc("T" i, d)
				tnm["T" i] = tname[i]
			}
		}

		p_t = t_now; p_usr = s_usr; p_sys = s_sys; p_idle = s_idle
		p_irq = s_irq; p_sirq = s_sirq; p_tot = s_tot
		p_ctxt = s_ctxt; p_intr = s_intr
		for (i in cj) { pj[i] = cj[i]; pu[i] = cu[i]; ps_[i] = cs[i] }
		for (i in tj) ptj[i] = tj[i]
		for (c in core_idle) pci[c] = core_idle[c]
	}

	# --------------------------------------------------------------- summary --
	printf "\n--- system (%% of one core) ---\n"
	printf "%-14s %7s %7s %7s %7s %7s\n", "metric", "mean", "min", "max", "range", "stdev"
	prow("total busy", "__busy")
	prow("  user", "__usr")
	prow("  system", "__sys")
	prow("  irq+softirq", "__sirq")
	for (c = 0; c < ncores; c++) prow("cpu" c " busy", "__core" c)
	prow("UNATTRIBUTED", "__unattr")
	printf "%-14s %7.0f %7.0f %7.0f %7.0f %7.0f\n", "ctxsw/s",
		mean("__ctxsw"), mn["__ctxsw"], mx["__ctxsw"],
		mx["__ctxsw"] - mn["__ctxsw"], stdev("__ctxsw")
	if (SPIN)
		printf "%-14s %7.1f %7.1f %7.1f %7.1f %7.1f   <- flat means stable clock\n", "spin_ms",
			mean("__spin"), mn["__spin"], mx["__spin"],
			mx["__spin"] - mn["__spin"], stdev("__spin")
	if ("__freq" in n_)
		printf "%-14s %7.0f %7.0f %7.0f %7.0f %7.0f   <- range 0 means no DVFS\n", "freq kHz",
			mean("__freq"), mn["__freq"], mx["__freq"],
			mx["__freq"] - mn["__freq"], stdev("__freq")

	printf "\nUNATTRIBUTED = busy minus every userspace+kthread row. On SigmaStar this is\n"
	printf "IRQ/softirq and in-kernel MI SDK pipeline work charged to no task.\n"
	printf "Small negative values are sampling skew (/proc/stat and the per-pid pass are\n"
	printf "not atomic, and processes that exit mid-window lose their jiffies).\n"

	# Processes ranked by stdev: the question is what *varies*, not what is big.
	cnt = 0
	for (k in n_) {
		if (substr(k, 1, 1) != "P") continue
		if (mx[k] < 0.3 && stdev(k) < 0.1) continue
		keys[++cnt] = k
		var[k] = stdev(k)
	}
	sortkeys(cnt, var, keys)
	printf "\n--- processes ranked by VARIANCE (stdev, %% of one core) ---\n"
	printf "%-20s %6s %7s %7s %7s %7s\n", "process", "pid", "mean", "min", "max", "stdev"
	for (i = 1; i <= cnt && i <= TOPN; i++) {
		k = keys[i]
		printf "%-20s %6s %7.1f %7.1f %7.1f %7.1f\n",
			substr(pnm[k], 1, 20), substr(k, 2), mean(k), mn[k], mx[k], stdev(k)
	}

	printf "\n--- same processes ranked by MEAN ---\n"
	for (i = 1; i <= cnt; i++) var2[keys[i]] = mean(keys[i])
	sortkeys(cnt, var2, keys)
	printf "%-20s %6s %7s %7s %7s %7s\n", "process", "pid", "mean", "min", "max", "stdev"
	for (i = 1; i <= cnt && i <= TOPN; i++) {
		k = keys[i]
		printf "%-20s %6s %7.1f %7.1f %7.1f %7.1f\n",
			substr(pnm[k], 1, 20), substr(k, 2), mean(k), mn[k], mx[k], stdev(k)
	}

	if (THREADS) {
		tcnt = 0
		for (k in n_) {
			if (substr(k, 1, 1) != "T") continue
			if (mx[k] < 0.3) continue
			tkeys[++tcnt] = k
			tvar[k] = mean(k)
		}
		sortkeys(tcnt, tvar, tkeys)
		printf "\n--- %s threads by mean (%% of one core) ---\n", TARGET
		printf "%-20s %6s %7s %7s %7s %7s\n", "thread", "tid", "mean", "min", "max", "stdev"
		for (i = 1; i <= tcnt; i++) {
			k = tkeys[i]
			printf "%-20s %6s %7.1f %7.1f %7.1f %7.1f\n",
				substr(tnm[k], 1, 20), substr(k, 2), mean(k), mn[k], mx[k], stdev(k)
		}
		printf "note: threads sum to the process row only; SDK kernel pipeline cost\n"
		printf "shows up in UNATTRIBUTED above, not here.\n"
	}
}

function prow(label, key) {
	printf "%-14s %7.1f %7.1f %7.1f %7.1f %7.1f\n", label,
		mean(key), mn[key], mx[key], mx[key] - mn[key], stdev(key)
}
'
