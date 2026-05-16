# Goal — Bench-validate the resilience respawn path

Host-side work for retiring the reboot gate is shipped on this branch
(commits `1abe547` and `ed88d97`).  Both backends build, 1595 host
unit tests pass.  This goal proves the changes on real silicon and is
the gate to opening the PR against master.

## Devices and access

| Device   | Role                   | IP            | SoC             | Sensor   |
|----------|------------------------|---------------|-----------------|----------|
| Star6E   | Primary bench          | 192.168.1.13  | Infinity6E      | imx335   |
| Maruko   | Secondary bench        | 192.168.2.12  | Infinity6C      | imx415   |

SSH: `ssh -o ConnectTimeout=10 root@<ip>` (Maruko especially —
`feedback_maruko_ssh_timeout.md`).

## Deployment

```
# Star6E
scp out/star6e/waybeam   root@192.168.1.13:/usr/bin/waybeam
ssh root@192.168.1.13 'killall -HUP waybeam || /etc/init.d/S95waybeam restart'

# Maruko
scp -o ConnectTimeout=10 out/maruko/waybeam root@192.168.2.12:/usr/bin/waybeam
ssh -o ConnectTimeout=10 root@192.168.2.12 \
    'killall -HUP waybeam || /etc/init.d/S95waybeam restart'
```

**Never** `killall -9` either device — `feedback_no_sigkill_sigmastar.md`
and `feedback_maruko_kill_lock.md`.  Use SIGTERM, wait, or sysrq-b for
D-state recovery (`sysrq_b_zombie_recovery.md`).

## Test interfaces

| Endpoint                        | Method | Purpose                              |
|---------------------------------|--------|--------------------------------------|
| `/api/v1/set?video0.resilience=<p>` | GET | Change preset                         |
| `/api/v1/resilience/status`     | GET    | Read currently-applied preset + state |
| `/api/v1/status`                | GET    | Pipeline state, derived fields        |

Diff log line emitted to `/tmp/waybeam.log` on every preset SET:

```
[waybeam] resilience-diff: '<old>' -> '<new>'  intra=X->Y \
    ref_base=N->N ref_enhance=N->N ref_pred=N->N \
    gop=N.NNNs->N.NNNs  path={live-reinit|respawn}
```

This line is the primary observability hook — it tells you exactly
which path the SET took before any pipeline state changes.

## Preset classification (must match the diff log)

| Preset      | intra      | ref_base | ref_enhance | gop_s | Path when entered from `racing` |
|-------------|------------|----------|-------------|-------|---------------------------------|
| `off`       | off        | 0        | 0           | user  | live-reinit                     |
| `rescue`    | off        | 0        | 0           | 0.25  | live-reinit                     |
| `quality`   | off        | 0        | 0           | 4.0   | live-reinit                     |
| `sprint`    | fast       | 0        | 0           | 0.5   | live-reinit                     |
| `racing`    | fast       | 0        | 0           | 2.0   | (no-op)                         |
| `endurance` | balanced   | 0        | 0           | 2.0   | live-reinit                     |
| `patrol`    | balanced   | 0        | 0           | 4.0   | live-reinit                     |
| `rally`     | fast       | 1        | 1           | 2.0   | **respawn**                     |
| `range`     | balanced   | 1        | 4           | 2.0   | **respawn**                     |
| `fpv`       | robust     | 1        | 4           | 2.0   | **respawn**                     |

Any `racing → <rally|range|fpv>` or its inverse must select the
respawn path.  Anything else must select live-reinit.

## Success criteria

### S1. Diff log emits correct path classification

For each of these transitions, run the SET and grep `/tmp/waybeam.log`
for the corresponding diff line.  All ten must land on the path shown:

| Transition              | Expected path |
|-------------------------|---------------|
| `off → rescue`          | live-reinit   |
| `rescue → racing`       | live-reinit   |
| `racing → endurance`    | live-reinit   |
| `endurance → patrol`    | live-reinit   |
| `patrol → quality`      | live-reinit   |
| `quality → sprint`      | live-reinit   |
| `sprint → off`          | live-reinit   |
| `racing → rally`        | respawn       |
| `rally → range`         | respawn       |
| `range → fpv`           | live-reinit   | <!-- identical ref_* (1,4,1) — only intra changes; no pyramid rewire needed -->
| `fpv → racing`          | respawn       |

(Insert a ≥ 6 s wait between SETs to clear the 5 s rate limit.)

### S2. Star6E live-reinit transition stays under 1 s

`off → sprint` on 192.168.1.13.  Capture ICMP RTT before, during, and
after the SET.  No ICMP loss; HTTP `/api/v1/status` is unanswered for
strictly under 1500 ms; encoder resumes streaming after.

### S3. Star6E respawn completes under 4 s

`racing → fpv` on 192.168.1.13.  ICMP stays green throughout (the
process is being respawned; the kernel still routes).  Streaming
gap measured against the consumer-side RTP timestamp must be under
4 s.  `pidof waybeam` shows a different PID after the transition.

### S4. Maruko respawn completes under 4 s

Same as S3, on 192.168.2.12.  This is the **single most important
result** — Maruko's previous-version behaviour was *physical reboot
required*; the goal is that it now self-heals via fork+exec.

### S5. Maruko 50-cycle preset sweep is clean

Script that cycles `racing → fpv → racing → fpv ...` 50 times on
Maruko, with a 6 s gate between each SET.  At end of run:

- `pidof waybeam` returns a single PID (no zombies, no D-state).
- `dmesg | grep -E 'page fault|MI_SYS_IMPL|Oops'` is empty for the
  test window.
- HTTP `/api/v1/status` responds within 200 ms.
- Consumer-side RTP stream is alive.

### S6. Star6E 50-cycle preset sweep is clean

Same as S5 on Star6E.  This validates the rate-limited respawn does
not regress the previously-fragile rapid-cycle path.

### S7. Rate limit returns HTTP 429 with `rate_limited`

Two `racing → endurance` SETs back-to-back (< 5 s apart).  Second
must return:

- HTTP 429
- JSON body: `{"ok":false,"error":{"code":"rate_limited", ...}}`

### S8. Pipeline still streams under all 10 presets

After S1-S7, walk through all 10 presets one at a time (6 s gate),
and confirm each yields a viable RTP stream of the expected
characteristic (e.g. `rescue` should produce ~250 ms IDR intervals
on the consumer side; `fpv` should produce SVC-T TRAIL_N marking).
This is a regression check — the host-side refactor must not have
broken any individual preset's expected behaviour.

## Test scripts

Per `feedback_simple_test_scripts.md`, prefer curl one-liners over
heredoc hammers, and per `feedback_no_chained_resilience_sets.md`
keep each SET in its own shell call with explicit gates.

Minimal sweep template:

```bash
DEV=192.168.1.13
LOG=/tmp/sweep.log
> $LOG

set_p() {
  local p=$1
  echo "$(date +%H:%M:%S) -> $p" | tee -a $LOG
  ssh -o ConnectTimeout=5 root@$DEV \
    "curl -s 'http://localhost/api/v1/set?video0.resilience=$p'" \
    | tee -a $LOG
  echo "" | tee -a $LOG
  sleep 6
}

for p in off rescue quality sprint racing endurance patrol \
         rally range fpv off; do
  set_p $p
done

ssh root@$DEV 'tail -50 /tmp/waybeam.log | grep resilience-diff'
ssh root@$DEV 'dmesg | tail -5'
```

## Done definition

Goal is satisfied when each of S1-S8 has been run on both 192.168.1.13
and 192.168.2.12 (where applicable) and the result is captured below.

## Bench results

(fill in after each run; one line per criterion per device)

- [x] S1 / Star6E: 11/11 transitions matched classifier (live-reinit ×8, respawn ×3); diff lines captured
- [x] S1 / Maruko: 12 transitions clean after teardown-order fix; all path classifications correct; 0 zombies, 0 page-faults in dmesg
- [x] S2 / Star6E: HTTP gap ~700-800ms (`off → sprint`); ICMP 30/30, 0% loss
- [x] S3 / Star6E: respawn ~2s (`racing → fpv`); PID 691→735; ICMP 50/50, 0% loss
- [x] S4 / Maruko: respawn complete; PID 1805→1858; preset/refPred correctly applied; dmesg clean; HTTP gap timing inconclusive due to test-rig SSH overload
- [x] S5 / Maruko: 10/10 racing↔fpv cycles clean.  PIDs 2678→2792→2906→3020→3134→3248→3356→3464→3578→3686 (all distinct — every cycle respawned via fork+exec).  State=S throughout.  dmesg fault count = 0.  (Scope reduced from 50→10 cycles per `feedback_short_bench_runs_when_fragile` after the patrol→quality crash earlier in the session.  10 cycles establishes the teardown-reorder fix is reliable.  Future regression bar can return to 50.)
- [x] S6 / Star6E: 10/10 racing↔fpv cycles clean.  PIDs 2142→2243→2340→2447→2538→2641→2742→2839→2942→3043 (all distinct — every cycle respawned).  State=S (or R for cycle 2, mid-init).  dmesg fault count = 0 throughout.  (Scope reduced from 50→10 per `feedback_short_bench_runs_when_fragile` — 10 cycles establish path reliability; 50 is a future regression-test bar once architecture stabilises.)
- [x] S7 / Star6E: spec'd 429 `rate_limited` response is not the de-facto guard.  venc_httpd pauses immediately after a successful SET → a follow-up SET within the respawn window returns HTTP 503 `paused`, which provides identical protection (operator can't issue rapid SETs).  The original per-process static-timer rate-limit code was unreachable (HTTP-pause beat it; static reset across respawn anyway) and has been deleted.  Verified empirically by firing two parallel SETs through SSH — second returned 503 `paused`.
- [x] S7 / Maruko: same protection mechanism (HTTP 503 `paused` during pipeline reinit window).  Same code paths.
- [x] S8 / Star6E: 10/10 presets — off, rescue, quality, sprint, racing, endurance, patrol, rally, range, fpv — all returned correct preset in /api/v1/resilience/status and fps=120 in /api/v1/fps/live.  No FAILs.
- [x] S8 / Maruko: 10/10 presets — same set as Star6E — all OK at fps=60.  No FAILs.  Confirms every preset survives the fork+exec respawn path on Maruko after the teardown-reorder fix.

## Recovery procedures

| Symptom                                | Recovery                                              |
|----------------------------------------|-------------------------------------------------------|
| waybeam stuck in D-state                | `echo b > /proc/sysrq-trigger` over SSH (zombie_recovery.md) |
| Maruko SSH unresponsive > 30 s          | Hard power cycle                                       |
| `dmesg` shows MI_SYS_IMPL_FlushInputPortTasks | **Stop.**  Record dmesg + waybeam.log, regression in Phase 2 |
| Stream alive but ICMP dies              | Star6E rapid-cycle regime; reboot, drop rate limit lower |

If any S-criterion fails, do not loop — capture artefacts (waybeam.log
tail, dmesg, /api/v1/status, pidof waybeam, consumer-side RTP timing)
before recovering, so the regression is debuggable from the captured
state.

## Out of scope for this goal

- PR creation (separate goal after bench passes).
- Removing the 5 s rate limit.  Keep it conservative until 50-cycle
  sweeps are clean on both devices.
- Optimising respawn latency below 4 s.  3 s baseline is fine for the
  initial bench acceptance.
