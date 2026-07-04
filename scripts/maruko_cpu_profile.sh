#!/bin/sh
PID=$(pidof waybeam)
[ -z "$PID" ] && { echo "no waybeam pid"; exit 1; }
INT=${1:-10}
HZ=100

read_cpu() { awk 'NR==1{u=$2+$3+$4+$7+$8; i=$5+$6; print u, i}' /proc/stat; }
snap_threads() {
  for s in /proc/$PID/task/*/stat; do
    tid=${s%/stat}; tid=${tid##*/}
    awk -v tid="$tid" '{ line=$0; sub(/^[0-9]+ \([^)]*\) /,"",line); split(line,a," "); print tid, a[12]+a[13] }' "$s" 2>/dev/null
  done
}

C0=$(read_cpu); T0=$(snap_threads)
sleep $INT
C1=$(read_cpu); T1=$(snap_threads)

set -- $C0; U0=$1; I0=$2
set -- $C1; U1=$1; I1=$2
DU=$((U1-U0)); DI=$((I1-I0)); DT=$((DU+DI))
echo "=== SYSTEM CPU over ${INT}s (1 core) ==="
awk -v du=$DU -v dt=$DT 'BEGIN{printf "busy=%.1f%%  idle=%.1f%%\n", du*100/dt, 100-du*100/dt}'
# split usr vs sys
set -- $(awk 'NR==1{print $2+$3, $4, $8}' /proc/stat)   # placeholder not used

echo "=== waybeam per-thread CPU% (>=0.5%) ==="
echo "$T0" | while read tid j0; do
  j1=$(echo "$T1" | awk -v t=$tid '$1==t{print $2}')
  [ -z "$j1" ] && continue
  dj=$((j1-j0))
  nm=$(cat /proc/$PID/task/$tid/comm 2>/dev/null)
  awk -v dj=$dj -v i=$INT -v hz=$HZ -v tid=$tid -v nm="$nm" 'BEGIN{p=dj/(i*hz)*100; if(p>=0.5) printf "%6.1f%%  tid=%-6s %s\n", p, tid, nm}'
done | sort -rn
echo "=== waybeam total (sum of thread utime+stime jiffies over window) ==="
awk -v pid=$PID '{ line=$0; sub(/^[0-9]+ \([^)]*\) /,"",line); split(line,a," "); print "proc-level utime+stime jiffies (cumulative):", a[12]+a[13] }' /proc/$PID/stat
