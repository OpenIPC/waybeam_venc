# Star6E — CPU profile and the `top` variance artifact

Investigation of an apparent "massive CPU variance, 1–15%" (later refined to
"2–25%, periodic") observed for the `waybeam` binary on the Star6E vehicle.
Device: **192.168.2.232**, SSC338Q, `openipc-ssc338q`, kernel 4.9.84,
**dual-core ARMv7 @ 1200 MHz**. Measured 2026-07-25 while streaming with
`waybeam-link tx` and `waybeam_hub` co-resident.

**Conclusion: there is no variance in `waybeam`. Over 5 minutes at 1 s
resolution it held 21.0–26.0% of one core (mean 22.9, stdev 1.2, max/mean
1.13x). The 2–25% swing seen in `top` is an artifact of this kernel's
`/proc/stat` accounting, which busybox `top` uses as its denominator.** Details
in [The `top` artifact](#the-top-artifact).

## Tooling

Two scripts, both pure busybox ash + awk, no `perf`, no procps:

| Script | Scope | Use when |
|---|---|---|
| `scripts/cpu_profile.sh` | whole system, all processes + target threads | "where does system CPU go, and what varies" |
| `scripts/waybeam_thread_watch.sh` | one binary, per-thread, high sample rate | "which thread inside waybeam, and does it burst" |

```sh
# deploy (scp is unreliable on these targets, pipe through cat)
cat scripts/cpu_profile.sh | ssh root@192.168.2.232 'cat > /tmp/cpu_profile.sh; chmod +x /tmp/cpu_profile.sh'

ssh root@192.168.2.232 '/tmp/cpu_profile.sh -n 40 -i 3 -t'          # system, 120 s
ssh root@192.168.2.232 '/tmp/waybeam_thread_watch.sh -i 0.5 -n 100' # threads, 50 s
```

Both report **"% of ONE core"**, so the dual-core Star6E saturates at 200% and
process rows are directly comparable to system rows.

> `scripts/maruko_cpu_profile.sh` is the older single-shot, single-core
> predecessor. It cannot show variance (one window only) and mislabels a
> 2-core box. Prefer `cpu_profile.sh` on Star6E.

## Measured — whole system

40 samples × 3 s, device otherwise untouched:

| Metric | mean | min | max | stdev |
|---|---|---|---|---|
| **total busy** | **68.0%** | 66.5 | 69.6 | **0.8** |
| user | 23.1 | 17.4 | 37.2 | 4.1 |
| system | 40.0 | 25.3 | 45.5 | 4.5 |
| cpu0 busy | 34.2 | 29.7 | 38.0 | 1.8 |
| cpu1 busy | 33.8 | 29.6 | 37.5 | 1.7 |
| UNATTRIBUTED | 4.9 | 3.1 | 6.3 | 0.9 |

Of a 200% budget the system sits at 68%, i.e. **~34% of total capacity**, evenly
spread across both cores, with a standard deviation of **0.8 points**. Nothing
here oscillates.

Per process (mean, % of one core):

| Process | mean | stdev | Note |
|---|---|---|---|
| `waybeam` | 23.0 | 0.7 | the encoder itself |
| `waybeam-link tx` | 12.5 | 0.3 | AF_PACKET raw inject + FEC |
| `IspDriverThread` | 7.3 | 0.2 | kernel, ISP 3A |
| `vpe0_P0_MAIN` | 6.9 | 0.2 | kernel, MI SDK pipeline |
| `venc0_P0_MAIN` | 2.7 | 0.2 | kernel, encoder |
| `waybeam_hub` | 2.0 | 0.2 | |
| `ai0_P0_MAIN` | 1.6 | 0.2 | kernel, audio in |
| `vif0_P0_MAIN` | 1.2 | 0.2 | kernel, sensor interface |
| `awk` | 4.4 | 0.1 | **the profiler itself** — subtract it |

`system` (40.0) exceeds `user` (23.1): most of the cost is kernel-side, matching
the Maruko finding that the MI SDK pipeline dominates. `UNATTRIBUTED` (4.9%) is
IRQ/softirq time charged to no task.

## Measured — `waybeam` threads

**300 samples × 1 s (5 minutes)**, to catch both short bursts and slow
periodicity. `waybeam` calls `pthread_setname_np` nowhere, so every thread's
`comm` is `waybeam`; they are identified by tid + `wchan`.

| tid | wchan | mean | min | max | stdev | Likely role |
|---|---|---|---|---|---|---|
| 2558 | `poll_schedule_timeout` | 14.0 | 12.6 | 15.4 | 0.5 | main loop: GetStream → HEVC/RTP → send |
| 2586 | `futex_wait_queue_me` | 7.2 | 6.7 | 8.7 | 0.6 | worker on mutex/condvar |
| 2585 | `poll_schedule_timeout` | 1.4 | 1.0 | 2.9 | 0.5 | secondary poll loop |
| 2589 | `hrtimer_nanosleep` | 0.3 | 0.0 | 1.0 | 0.4 | periodic timer (beacon / ramp / AE) |
| 2563 | `inet_csk_accept` | 0.1 | 0.0 | 1.0 | 0.3 | HTTP accept loop (`venc_httpd.c`) |
| 2579 | `futex_wait_queue_me` | 0.0 | 0.0 | 0.0 | 0.0 | idle worker |
| 2582 | `pipe_wait` | 0.0 | 0.0 | 0.0 | 0.0 | stdout log filter (`audio_codec.c`) |
| **total** | | **22.9** | **21.0** | **26.0** | **1.2** | **max/mean = 1.13x** |

Two threads account for essentially all of it: the main loop (14%) and one
worker (7%). Over 300 consecutive samples the process total never left
21.0–26.0%, and the handful of samples above mean+2σ are all exactly 26.0% —
one-jiffie quantisation noise (at 1 s, 1 jiffie ≈ 1%), not bursts. A separate
100 × 0.5 s run agreed (18.2–29.1%, stdev 1.9; the wider spread is just coarser
quantisation in a shorter window).

**Nothing inside `waybeam` oscillates on any timescale from 0.5 s to 5 min.**

## The `top` artifact

**`/proc/stat` on this kernel is unreliable over short windows.** Sampling it
across identical 2.02 s windows, where the true available budget is
`2.02 × 100 × 2 = 404` jiffies:

| Counter | observed range over 2 s windows | error |
|---|---|---|
| `dt` (wall, `/proc/uptime`) | 2.01–2.03 s | stable |
| **waybeam's own jiffies** | **44–49** | **stable, ±5%** |
| `/proc/stat` total (all 8 fields) | **287–554** | −29% … +37% |
| `/proc/stat` busy fields | **26–323** | **12x swing** |
| `/proc/stat` idle field | 259–286 | stable |

Broken out per field, the first line of `/proc/stat` being
`cpu user nice system idle iowait irq softirq steal guest guest_nice`:

| field | delta range over 2 s (budget 400) | |
|---|---|---|
| `idle` | **273–279** | rock stable |
| `system` | **6 → 236** | **39x swing** |
| `user` | 4 → 118 | ~30x swing |
| `softirq` | 0–18 | small |
| `nice` / `irq` / `iowait` / `steal` | ~0 | unused on this SoC |
| **busy sum** | **11 → 263** | **24x swing** |

`user` and `system` burst **together** (one window user=118/system=127, a quiet
one user=5/system=6) while `idle` never moves. In the worst window the fields
claim 263 busy jiffies while `idle` allows only ~123, i.e. the kernel reports
"131% of one core" and "61% of one core" for the same 2 s.

### The busy time is real — it is credited late, to the wrong bucket

The fields are **not** inventing load. Comparing three independent estimates of
busy jiffies over one window, at three window sizes:

| window | `FIELD` | `IDLE` (ref) | `PER-TASK` (ref) | FIELD/IDLE |
|---|---|---|---|---|
| 2 s | 14 | 130 | 115 | **0.11** |
| 10 s | 597 | 628 | 425 | 0.95 |
| 60 s | 2958 | 3749 | 3483 | 0.79 |

Over 2 s the fields report **11%** of the true busy time; widen the window and
they land within 5–21% of the reference. A kernel fabricating load would show
the same error at every window size. Instead it largely averages out — the
signature of **delayed, batched crediting**: work done in one short window is
booked in a later one. The single-window over-report above is late credit for
earlier work, not work that never happened.

So the defect is **misattribution, in two dimensions**:

- **In time** — recoverable by measuring over ≥10 s, or by using `idle` (which
  tracks real time) instead of the busy fields.
- **In category** — *not* recoverable at any window size. A syscall-free shell
  loop cannot generate system time, yet `system` swung 10 → 319 for exactly such
  a load (see below). Treat the `usr`/`sys` split on this kernel as
  indicative only.

`idle` and the per-task counters are the two trustworthy sources; they agree
with each other and disagree with the busy fields.

This is a kernel accounting defect, **not** a workload effect and **not**
attributable to `waybeam`: the binary's own per-task counters are flat (44–49
jiffies), no process in a 40-sample sweep had a stdev above 0.7 points, and a
process cannot influence how the kernel buckets time into these global fields.
The burst pattern is consistent with accumulated time being flushed in batches
rather than per tick, but that mechanism is unconfirmed in the kernel source.

### Proof by constant synthetic load

Killing `waybeam` does make `top` settle at a stable 90–100% idle — but that
alone proves nothing, because it removes the real work *and* anything to
mis-account. The discriminating test is a **provably constant** load. Running a
shell `while : ; do : ; done` loop (pure userspace, essentially zero syscalls)
alongside `waybeam`:

| quantity | range over 2 s windows | |
|---|---|---|
| spinner own jiffies | **173–177** | constant by construction |
| `waybeam` own jiffies | **44–47** | constant |
| `idle` delta | 95–111 | stable |
| **busy derived from `idle`** | **300–315** | **stable, 5%** |
| **busy derived from the fields** | **224–412** | **1.8x swing** |
| `user` field | 72 → 298 | incoherent |
| `system` field | **10 → 319** | **incoherent** |

A shell busy-loop cannot generate system time, yet `system` swings 10 → 319
while the loop's own counter never moves. **The kernel re-buckets a constant
workload at random between `user` and `system`.** Since a `while` loop is
self-evidently not `waybeam`, the defect is kernel-side, confirmed.

Corollary: the defect **needs load to manifest** — it mis-buckets work that
exists, so an idle box looks fine. And because the absolute field error is
roughly fixed, the *relative* swing grows as real load shrinks, which is how a
steady 23% renders as 2–25% in `top`.

Busybox `top` divides each process's jiffie delta by a `/proc/stat`-derived
total, so it inherits that noise:

```
dt=2.02 dproc=47 dtot=363 | wall%=23.3  top%=12.9
dt=2.02 dproc=48 dtot=518 | wall%=23.8  top%= 9.3   <- dproc flat, top% halved
dt=2.03 dproc=48 dtot=287 | wall%=23.6  top%=16.7   <- dproc flat, top% doubled
dt=2.02 dproc=47 dtot=554 | wall%=23.3  top%= 8.5
```

`dproc` never moves; `top%` swings ~2x against the total and up to **~12x**
against the busy fields — which is exactly the 2–25% range reported. A single
`top` snapshot on this SoC also read `94.0% idle` and `waybeam 2.3%` at a moment
when direct measurement showed 68% busy and waybeam at 23%: a **10x
under-report**.

## The debug OSD `cpu NN%` readout (fixed)

The built-in debug OSD had the identical bug. `src/debug_osd.c`'s shared
`OsdCpuSampler` (used by the Star6E, Maruko and CV610 backends, rendered as
`cpu NN%` from `star6e_runtime.c:1457` / `maruko_pipeline.c:4255` /
`cv610_runtime.c:1042`) computed:

```c
total = user + nice + sys + idle + iowait + irq + softirq;  /* the bad sum */
dt = total - ring[oldest].total;
pct = (dt - di) * 100 / dt;                                 /* over ~1 s */
```

— a field-sum denominator over a ~1 s window, i.e. the worst possible regime.

**Fix:** derive busy from `idle` against a wall-clock denominator, and widen the
window to ~2 s:

```c
avail = span_ms * USER_HZ * ncores / 1000;   /* capacity in jiffies */
busy  = avail - delta_idle;
pct   = busy * 100 / avail;
```

The bursty fields are no longer read at all. `ncores` is counted from the
`cpuN` lines in `/proc/stat` rather than `sysconf(_SC_NPROCESSORS_ONLN)`, since
the Maruko backend builds against musl; `USER_HZ` comes from
`sysconf(_SC_CLK_TCK)` with a fallback of 100. Semantics are unchanged — still
a percentage of total capacity across all cores.

Both samplers simulated on .232 at the same instants, 60 × 0.5 s:

| | mean | min | max | range | **stdev** |
|---|---|---|---|---|---|
| **OLD** (field sum, ~1 s) | 19.1% | **2** | **54** | **52** | **18.0** |
| **NEW** (idle + wall clock, ~2 s) | 31.1% | 30 | 32 | 2 | **0.5** |

The old readout swung 2–54% and **under-reported the mean by a third** (19.1 vs
31.1), consistent with the field deficit measured above. The new one holds
±1 point.

### CV610 breaks the other way — and needs a different source

Everything above is Star6E-specific. The CV610 (Hi3516CV6xx, 5.10) fails in the
**opposite** direction, so the fix above is a no-op there.

On Star6E the /proc/stat *total* is inconsistent (287–554 jiffies against a
404-jiffy budget) and `idle` is the stable anchor. On CV610 the total is
**exactly** right — `Δfields + Δidle == avail` in every window — so idle-derived
and field-derived are algebraically the same number. Measured on .181, both
read 29.5 mean / 2.4–71.6 / stdev 23.2, identical to a decimal. It is the
idle/busy **split** that oscillates, not the total, so choosing a different half
buys nothing. A window sweep needs **30 s** before stdev settles (1.5), and even
then reads 26% against a true 36.5%.

Mechanism: the aggregate `/proc/stat` line is purely tick-sampled, while
per-task `utime/stime` are rescaled to the ns-accurate `sum_exec_runtime`
(`cputime_adjust`) — which is why per-task is flat and the aggregate is not.
venc runs **100 fps against HZ=100**, so the tick keeps landing on the same
phase of every frame; the beat between the two clocks is the ~20–30 s cycle.

**Source: `/proc/schedstat`.** Each `cpuN` row is
`"cpu%d %u 0 %u %u %u %u %llu %llu %lu"` — six counters after the label, THEN
`rq_cpu_time` in nanoseconds, i.e. **field 8, not 7** (field 7 is one of the
zeros; an awk sum over `$7` returns 0, which reads as a permanently idle box).
That is real task-execution time off the scheduler clock:

| source (.181, same 39 s) | mean | min | max | **stdev** |
|---|---|---|---|---|
| `/proc/stat` idle-derived | 15.0 | 1.9 | 95.3 | **22.05** |
| `/proc/schedstat` `rq_cpu_time` | 34.4 | 30.1 | 38.3 | **2.02** |
| all-task sum (independent truth) | 32.3 | 28.0 | 35.8 | **2.08** |

**Star6E must NOT switch to schedstat.** Measured on .232, idle-derived reads
**60.5** while the all-task sum reads **52.7** — a **7.7-point** gap of real
IRQ/softirq work charged to no task. `rq_cpu_time` counts only task execution,
so it would under-report Star6E by exactly that gap. The 4.9 kernel has no
`CONFIG_SCHEDSTATS` so it could not switch anyway, but the sampler does not
rest the invariant on that: the schedstat branch is **compile-time
`PLATFORM_CV610` only**. The same reasoning excludes the ground station — on
x86 (12 cores) `/proc/stat` reads 2.20 against schedstat 1.97 and an all-task
truth of 1.26, all three steady, so there is no defect there to fix and
switching would only shift the number.

Two properties of the file to respect. Schedstat lists only *online* CPUs, so
counting `cpuN` rows gives the correct denominator directly — but a consumer
that keeps a running total across samples must handle that total **stepping
down** when a core goes offline, which reads as a backwards counter. Treat a
backwards accumulator as *no measurement* and keep the previous value: feeding
a zero delta to the formulae fabricates opposite extremes (0% on the schedstat
path, 100% on the idle path).

Verified no-op on .232 (before → after deploying the schedstat build):
`@cpu_total` 60.5 → 59.1, still tracking the recomputed idle-derived value
(60.4 → 60.6), with the all-task sum unchanged at 52.7 → 52.6.

### Rules for reading CPU on Star6E

1. **Do not trust busybox `top` percentages.** Use `cpu_profile.sh` /
   `waybeam_thread_watch.sh`, which normalise per-task jiffies against wall
   clock from `/proc/uptime`.
2. **Derive system busy from `idle`**, never from the sum of the busy fields.
   `busy = (dt × HZ × ncores − Δidle)`. Using the field sum put this profiler's
   `UNATTRIBUTED` at **−96%**; switching to idle-derived brought it to **+4.9%**.
3. **`top -b -n1` reports averages since boot**, not instantaneous — always
   take the second or later iteration.
4. **Ignore load average.** It reads ~14 while the box is 34% busy: the MI SDK
   kernel threads (`vpe0_P0_MAIN`, `venc0_P0_MAIN`, `vif*`, `ai0`, …) park in
   uninterruptible **D-state**, which counts toward loadavg at ~0 CPU.
5. **`usr` vs `sys` splits are noisy at short intervals** even from per-task
   counters, because tick sampling charges whole ticks to one bucket. The *sum*
   is reliable; use ≥30 samples before trusting the split.

## Ruled out

- **DVFS / thermal throttling.** `scaling_governor=performance`,
  `scaling_cur_freq` pinned at **1200000 kHz** with range **0** across every
  sample; `/sys/class/thermal/` has no zones on this SoC. The clock does not move.
- **Process respawn.** PID churn seen early on (1325 → 1635 → 1897 → 2078) was
  the operator restarting the binary by hand, not a crash loop. Fresh starts do
  cost more (sensor + ISP bring-up), so a restart genuinely perturbs a reading —
  `waybeam_thread_watch.sh` aborts with `!! pid N disappeared` rather than
  silently mixing two processes.

## Notes for future profiling on this platform

- **Busybox awk here is built without math support** — `sqrt()` fails with
  `Math support is not compiled in`. Both scripts carry a Newton-Raphson
  `isqrt()`. Assume the same for `sin`/`log`/`exp`.
- **The observer is not free.** The full-system profiler costs ~4.4% of one
  core (mostly its per-sample `cat` fork); it reports its own `awk` row so you
  can subtract it. An awk spin loop costs ~8 µs/iteration, so
  `cpu_profile.sh -s` at the original 300k iterations burned **2.3 s per
  sample** and swamped the measurement — it is now 20k iterations and stays
  opt-in.
- **Sample fast enough.** A 3 s window averages a 0.5 s burst to nothing. Use
  `-i 0.5` or lower when hunting bursts; `-i 3` or higher for steady-state cost.
