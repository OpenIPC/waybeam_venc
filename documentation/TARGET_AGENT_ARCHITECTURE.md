# Target-Side Test Agent Architecture

**Status:** Design document. Not yet implemented.

This document describes a lightweight agent process that runs on the embedded
target device to replace SSH as the primary transport for deployment testing.
It addresses reliability and performance limitations of the current SSH-based
workflow documented in `REMOTE_TEST_WORKFLOW.md`.

## Motivation

The current `remote_test.sh` workflow uses multiple sequential SSH sessions
per test cycle (probe, stop majestic, detect family, copy binaries, chmod,
run, liveness, dmesg). SSH ControlMaster multiplexing reduces the
connection overhead, but structural limitations remain:

1. **SSH consumes target resources.** On constrained SoCs (64-128MB RAM),
   the SSH daemon and per-session forking compete with the test binary for
   memory and CPU.
2. **No out-of-band recovery.** When `venc` crashes the kernel or wedges
   the ISP driver, SSH dies with it. The agent has no way to determine
   *what happened* — it only knows SSH stopped responding.
3. **Fragile transfer.** `scp` and `sftp` are unavailable on many target
   images. The workaround (`ssh cat > file`) works but cannot resume
   partial transfers or verify integrity.
4. **No persistent state.** Each SSH session is stateless. There is no way
   to maintain a session across a target reboot, or to queue a test that
   should run after reboot completes.

## Architecture Overview

```
┌─────────────────┐          TCP :9900          ┌─────────────────────┐
│  Host (dev box)  │ ◄──────────────────────────► │  Target (embedded)   │
│                  │                              │                      │
│  remote_test.sh  │  ── upload binary ──────►    │  test_agent          │
│  (or agent CLI)  │  ── run command ─────────►   │   ├─ file receiver   │
│                  │  ◄── stdout/stderr stream ── │   ├─ process runner  │
│                  │  ◄── health report ───────── │   ├─ health monitor  │
│                  │  ◄── dmesg stream ────────── │   └─ watchdog        │
└─────────────────┘                              └─────────────────────┘
```

The target-side `test_agent` is a single static binary (C, ~20KB) that:

- Listens on a TCP port (default 9900).
- Accepts file uploads with SHA256 integrity verification.
- Spawns test binaries as child processes with configurable environment.
- Streams stdout/stderr back to the host in real time.
- Monitors child process health (exit code, signals, resource usage).
- Periodically reports target health (free memory, CPU load, dmesg tail).
- Survives test binary crashes (runs as a separate process group).
- Can trigger a controlled reboot and report when it's back.

## Protocol

Simple line-based text protocol over TCP. No TLS (these are local-network
test benches, not production systems).

### Commands (host → target)

```
PING
  → PONG <uptime_sec> <free_mem_kb>

UPLOAD <path> <size_bytes> <sha256>
  → READY
  (host sends raw bytes)
  → OK <sha256_verified>
  → ERR <reason>

RUN <path> [args...]
  → STARTED <pid>
  (target streams: STDOUT <line>, STDERR <line>)
  → EXIT <code> <signal> <duration_ms>
  → TIMEOUT <duration_ms>

KILL <pid>
  → KILLED <pid>

DMESG [since_line]
  → DMESG_START
  (target sends lines)
  → DMESG_END <total_lines>

HEALTH
  → HEALTH <json>
  {"uptime_sec":1234,"free_mem_kb":45000,"load_1m":"0.5","dmesg_lines":500}

CHMOD <path> <mode>
  → OK

REBOOT
  → REBOOTING
  (connection drops; host reconnects after delay)
```

### Stream Framing

Each message is a single line terminated by `\n`. Binary file uploads are
the only exception: after `READY`, the host sends exactly `size_bytes` of
raw data, followed by the target's `OK` or `ERR` response.

## Target Agent Implementation Notes

### Build Requirements

- C99, statically linked (no libc dependency on target beyond what the
  kernel provides — or linked against the target's musl/uClibc).
- Cross-compiled with the same toolchain used for `venc`.
- No threads — single event loop using `poll(2)` or `select(2)`.
- Must work on both glibc (Star6E) and musl/uClibc (Maruko) targets.

### Process Isolation

The agent MUST run in a separate process group from the test binary.
When the test binary is killed (SIGKILL, crash, timeout), the agent
must remain alive. This is the key advantage over SSH: the reporting
channel survives the failure.

```c
// Fork test binary in its own process group
pid_t pid = fork();
if (pid == 0) {
    setpgid(0, 0);  // new process group
    execve(path, argv, envp);
    _exit(127);
}
// Agent continues to serve the TCP connection
```

### Watchdog

The agent implements a simple software watchdog:

1. Before spawning a test binary, record a timeout deadline.
2. If the child does not exit before the deadline, send SIGTERM, wait 2s,
   then SIGKILL.
3. Report `TIMEOUT` to the host with the elapsed duration.

This is more reliable than the host-side `timeout` command, which depends
on SSH remaining connected.

### Health Monitoring

The agent periodically (every 5s during a test run) reads:

- `/proc/meminfo` — free memory
- `/proc/loadavg` — CPU load
- `dmesg` (via `/dev/kmsg` or `klogctl`) — new kernel messages

If free memory drops below a configurable threshold (default 8MB), the
agent kills the test binary preemptively and reports an OOM-risk condition.

### Persistence Across Reboot

The agent binary should be installed to a non-volatile location
(`/usr/bin/test_agent` or `/etc/init.d/` auto-start). After a reboot, the
agent starts automatically and the host can reconnect without SSH.

For `/tmp`-only deployments, the host must re-upload the agent after each
reboot (same as current binary deployment). A boot script hook can be added
to auto-start if the binary exists.

## Host-Side Integration

### Phase 1: Parallel with SSH

Initially, `remote_test.sh` gains a `--transport agent` flag (default
remains `ssh`). When `--transport agent` is used:

- File upload uses the agent's `UPLOAD` command instead of `ssh cat >`.
- Binary execution uses `RUN` instead of `ssh exec`.
- Health checks use `PING`/`HEALTH` instead of `ssh echo alive`.
- Dmesg collection uses `DMESG` instead of `ssh dmesg`.

The build step remains local (unchanged).

### Phase 2: Agent-Only

Once the agent transport is validated, SSH can be removed from the test
path entirely. SSH remains available for interactive debugging but is no
longer on the critical path for automated testing.

### Phase 3: Autonomous Test Sweeps

With a reliable agent transport, the host can queue multi-run test sweeps
(like `fps_sweep_test.sh`) as a single batch command. The agent executes
runs sequentially, reports results for each, and handles inter-run cleanup
(kill leftover processes, settle delays) locally. This eliminates the
round-trip latency between runs.

## Performance Comparison

| Metric | SSH (no mux) | SSH (ControlMaster) | Target Agent |
|--------|-------------|--------------------:|-------------:|
| Connection setup | ~300ms/call | ~5ms/call | 0 (persistent) |
| File transfer overhead | per-file SSH | single mux session | bulk upload |
| Calls per test cycle | ~8 | ~8 | ~3 (upload, run, health) |
| Survives test crash | no | no | yes |
| Out-of-band health | no | no | yes |
| Target RAM usage | ~2MB/session | ~2MB shared | ~200KB |

## Security Considerations

- The agent listens on a local network only. No authentication is
  implemented — this is appropriate for isolated test benches.
- If the test bench is on a shared network, add a simple shared-secret
  token to the protocol header.
- The agent can execute arbitrary binaries. This is by design (it's a
  test harness), but it should never be deployed on production devices.

## File Layout (Future)

```
star6e-standalone/
  tools/
    test_agent.c          # Target-side agent source
    test_agent_protocol.h # Protocol constants
  scripts/
    remote_test.sh        # Updated with --transport agent support
    agent_client.sh       # Thin host-side client for the agent protocol
```

## Implementation Priority

This is a **deferred** implementation. The current SSH + ControlMaster
approach (implemented in `remote_test.sh`) is adequate for the current
development pace. The target agent becomes worthwhile when:

1. Test frequency increases to multiple cycles per hour.
2. Board hangs become frequent enough that out-of-band diagnostics matter.
3. Fully autonomous test sweeps (without human power-cycle intervention)
   are needed.
4. Multiple agents need to test on the same device concurrently.
