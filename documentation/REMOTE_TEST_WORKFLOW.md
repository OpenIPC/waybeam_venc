# Remote Test Workflow

This document covers the bounded `remote_test.sh` workflow for CLI-driven
probes and short-lived test binaries across Star6E and Maruko backends.

For the current Star6E `venc` runtime path, prefer the direct deploy helper:

```bash
./scripts/star6e_direct_deploy.sh cycle
```

That path validates the production `/etc/venc.json` config, daemon startup,
HTTP API readiness, and persistent `/tmp/venc.log`. `remote_test.sh` remains
useful for sensor mode discovery, max-FPS sweeps, and auxiliary test binaries.

## Target Device

- Default target in script: `root@192.168.1.11` (legacy Maruko bench)
- Current Star6E venc bench: `root@192.168.1.13` via `star6e_direct_deploy.sh`
- Star6E remote-test bench example: `root@192.168.1.13`
- Maruko bench currently used: `root@192.168.2.12`
- For `root@192.168.2.12`, use longer SSH settings:
  `SSH_CONNECT_TIMEOUT=7 SSH_PROBE_TIMEOUT=12 make remote-test ...`
- Test directory on target: `/tmp/waybeam_venc_test`
- `/tmp` is volatile; after reboot, always re-upload test binaries.
- `remote_test.sh` auto-detects target family (`infinity6e` / `infinity6c`) and
  picks matching build target (`SOC_BUILD=star6e|maruko`).
- For deterministic runs, prefer explicit flags:
  - `--host root@192.168.1.13 --soc-build star6e`
  - `--host root@192.168.2.12 --soc-build maruko`
- For Maruko runs, always pass ISP bin:
  `--isp-bin /etc/sensors/imx415.bin`.
  `remote_test.sh` now injects this automatically for Maruko when not provided.
- Default stream destination for host-side validation:
  `-h 192.168.1.2`.

## Cold-State Rule

- If `majestic` has been run, reboot before claiming any cold-state result.
- Typical reboot/reconnect time is about 10 seconds.

## Safe Execution Pattern

1. Build locally: `make build`
2. Upload test artifacts to target `/tmp/waybeam_venc_test`.
   Star6E only uploads the test binary. Maruko uploads the test binary plus
   the vendored Maruko SDK libs, uClibc runtime, and shim into
   `/tmp/waybeam_venc_test/lib`.
3. Run tests with bounded timeout.
4. Check SSH liveness immediately after each run.
5. Inspect `dmesg` delta for sensor/ISP/VIF/VENC-related kernel messages.

This flow is intentionally best-effort for short-lived processes. It is not
the preferred way to validate the long-running `venc` daemon on Star6E.

`make stage` builds and bundles binaries **and** libs into `out/` for manual
provisioning or firmware image preparation. `remote_test.sh` uses the staged
binary-only path for Star6E, while Maruko deployments now also copy the
bundled Maruko runtime set to the target automatically.

Use helper script:

```bash
./scripts/remote_test.sh --help
```

SoC override is available when needed:

```bash
./scripts/remote_test.sh --soc-build maruko --run-bin venc -- --list-sensor-modes --sensor-index 0
```

## SSH ControlMaster

`remote_test.sh` and `fps_sweep_test.sh` use SSH ControlMaster multiplexing
to open one persistent TCP connection per host. All subsequent SSH calls in
the same script invocation reuse this connection, eliminating ~14 redundant
TCP+auth handshakes per test cycle.

- The control socket is created in a temporary directory and cleaned up on
  script exit (via `trap`).
- `ControlPersist=120` keeps the master alive for 2 minutes after the last
  use, which covers inter-run settle delays.
- If the target reboots mid-script (e.g., `--reboot-before-run`), the old
  master socket is stale. `fps_sweep_test.sh` re-establishes the mux after
  reboot; `remote_test.sh` falls back to fresh connections for the
  wait-for-SSH loop.

No user configuration is required. The mux is transparent.

## Exit Codes

`remote_test.sh` uses strict exit codes:

| Code | Meaning |
|------|---------|
| 0 | Run completed successfully |
| 1 | Binary exited non-zero |
| 2 | Device unresponsive (needs power cycle) |
| 124 | Run timed out |

Previously the script always exited 0 unless the device was unresponsive.
The new behavior allows callers and agents to act on the result
programmatically.

## JSON Summary

Pass `--json-summary` to emit a machine-readable JSON line after all
human-readable output:

```bash
./scripts/remote_test.sh --json-summary --host root@192.168.1.13 -- --list-sensor-modes --sensor-index 0
```

Output (last line):

```json
{"status":"success","exit_code":0,"device_alive":true,"dmesg_hits":0,"duration_sec":12,"run_bin":"venc","soc_build":"star6e","host":"root@192.168.1.13"}
```

See `AGENTS.md` → Deployment Test Interpretation for field definitions.

## Quick-Deploy Modes

Star6E assumes runtime libs already live in `/usr/lib` on the target and only
uploads test binaries. Maruko uploads the bundled runtime set together with
the binary unless deployment is skipped. Two flags control what gets rebuilt
and transferred:

| Flag | Effect |
|------|--------|
| `--skip-build` | Skip the `make clean && make all` step. Use when you've already built locally. |
| `--skip-deploy` | Skip all file transfer. Re-run the already-deployed binary with different arguments. |

These flags can be combined. Example: `--skip-build --skip-deploy`
re-runs the deployed binary without rebuilding or transferring anything.

## Transport Validation Notes

- `-m compact` sends raw UDP payload chunks (not RTP packetized).
- `-m rtp` sends proper RTP packetization and should be used for RTP receivers.
- For receiver-side validation, ensure decode pipeline matches selected transport.

## Transfer Note

Some target images do not have a working `sftp-server`; `scp` can fail.
When this happens, use plain `ssh` stream copy (for example `cat > file`).

## Hang Handling

- If timeout expires and SSH does not recover, treat the board as potentially hung.
- Ask for a power cycle before continuing further tests.

## Crash / Hang Tracking

Whenever venc crashes the SoC or causes a hang (SSH unresponsive, requires power cycle), **log the incident** with:

1. **What was run** — exact command line flags (`-s`, `-f`, `--sensor-mode`, etc.)
2. **Circumstances** — cold boot vs warm restart, prior majestic run, ISP bin loaded or not
3. **Behavior** — immediate hang, delayed hang, kernel panic in dmesg, or silent death
4. **Backend** — star6e or maruko, sensor type

Log entries go in `documentation/CRASH_LOG.md`.

**Three-strike rule:** When three or more entries share a similar pattern (same flag combination, same sensor transition, same pipeline stage), tag that pattern for deeper investigation and root-cause fix. Unless the pattern is critical enough to address immediately, it can be deferred to the next cycle.

## Future: Target-Side Test Agent

A design for replacing SSH with a purpose-built target-side agent is
documented in `documentation/TARGET_AGENT_ARCHITECTURE.md`. This is a
deferred implementation — the current SSH + ControlMaster approach is
adequate for now. The target agent becomes relevant when test frequency,
hang recovery, or autonomous sweep requirements outgrow SSH's capabilities.
