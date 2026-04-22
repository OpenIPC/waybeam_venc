# Project Instructions

This file is the single source of truth for all AI coding agents (Claude Code,
OpenAI Codex, or any tool that reads AGENTS.md / CLAUDE.md).

## Project Overview

Standalone SigmaStar video encoder with dual-backend architecture.

- Active implementation: `src/`
- Canonical documentation: `documentation/`
- Version tracking: `VERSION` (SemVer) + `HISTORY.md` (changelog)

## Development Workflow: spec - draft - simplify - verify

Every non-trivial task MUST follow this four-phase pipeline.
Each phase is a gate: do not advance until the current phase passes.

### Phase 1: Spec (Plan)

Before writing any code, produce a plan:

1. Read relevant documentation in `documentation/`.
2. Read the source files you intend to modify.
3. Write a concise plan covering: what changes, which files, why.
4. Document key design decisions and their rationale in the plan. This
   prevents oscillating between approaches mid-implementation.
5. Get human approval on the plan before proceeding.

Do NOT skip planning. A good plan lets you one-shot the implementation.

### Phase 2: Draft (Implement)

Execute the plan:

- Follow the coding conventions below.
- Make minimal, focused changes. Do not refactor unrelated code.
- Do not add features beyond what the spec calls for.
- **Verify incrementally**: run `make lint` after each logical change (new
  function, changed struct, modified pipeline stage). Do not batch all
  verification to Phase 4. Catching errors early prevents compounding
  failures that are harder to diagnose.
- If a change touches both backends, build each backend separately after
  the change: `make build SOC_BUILD=star6e`, then
  `make build SOC_BUILD=maruko`. Both output to separate directories
  (`out/star6e/`, `out/maruko/`) — no clean needed between them.

### Phase 3: Simplify (Review)

After implementation, review your own work:

- Can any function be shorter or clearer?
- Are there unnecessary abstractions, error paths, or comments?
- Does the architecture stay clean? No dead code, no orphan headers.
- Remove anything that is not strictly needed.

### Phase 4: Verify (Build + Test)

Run verification before declaring done:

```
make verify
```

This builds both backends and checks that all expected binaries exist.
If verify fails, follow the **Error Recovery Loop** below. Do not skip this step.

When connected devices are available, run targeted deployment tests after
`make verify` passes. Only rows with a **Host** filled in under
**Operational Defaults → Deployment Targets** are tested; blank rows are
skipped automatically.

For `venc` on the current Star6E bench (`root@192.168.1.13` / imx335), prefer
the direct JSON-config helper first:

```bash
scripts/star6e_direct_deploy.sh cycle
```

This validates the production `/etc/venc.json` path, daemon startup, HTTP API,
and `/tmp/venc.log` capture.

Use `make remote-test` for bounded CLI validation:

- sensor capability discovery (`--list-sensor-modes`)
- max-FPS sweeps across reported modes
- Maruko runtime deployment runs that still depend on `/tmp` staging

For each host row that uses `remote-test`, first probe sensor capabilities
with `--list-sensor-modes`, then test **every listed sensor mode** at its
reported max FPS. This catches per-mode regressions and cold-boot unlock
failures.

```
# 1) List sensor modes
make remote-test ARGS='--host root@<HOST> --soc-build <backend> --run-bin venc -- --list-sensor-modes --sensor-index <idx> [--isp-bin <path>]'

# 2) Test each mode at its max FPS (repeat for every mode reported above)
make remote-test ARGS='--host root@<HOST> --soc-build <backend> --run-bin venc -- --sensor-index <idx> --sensor-mode <M> -f <MAX_FPS> [--isp-bin <path>]'
```

**Example** — Star6E / imx335 at `192.168.1.13`, where `--list-sensor-modes`
reports modes 0 (30fps), 1 (60fps), 2 (90fps), and 3 (120fps):

```
make remote-test ARGS='--host root@192.168.1.13 --soc-build star6e --run-bin venc -- --list-sensor-modes --sensor-index 0'
make remote-test ARGS='--host root@192.168.1.13 --soc-build star6e --run-bin venc -- --sensor-index 0 --sensor-mode 0 -f 30'
make remote-test ARGS='--host root@192.168.1.13 --soc-build star6e --run-bin venc -- --sensor-index 0 --sensor-mode 1 -f 60'
make remote-test ARGS='--host root@192.168.1.13 --soc-build star6e --run-bin venc -- --sensor-index 0 --sensor-mode 2 -f 90'
make remote-test ARGS='--host root@192.168.1.13 --soc-build star6e --run-bin venc -- --sensor-index 0 --sensor-mode 3 -f 120'
```

Substitute `<HOST>`, `<backend>`, `<idx>`, and `--isp-bin` from the
Deployment Targets table. The mode indices and FPS values are examples for
imx415; always use the actual values reported by `--list-sensor-modes`.

If any mode's max-FPS run fails, check whether the sensor unlock sequence
fired (see `documentation/SENSOR_UNLOCK_IMX415_IMX335.md`).

Do not block a PR solely because a device is offline — only rows with a host
are tested.

For a full pre-PR check (version bump, changelog, build):

```
make pre-pr
```

## Remote-Test Interpretation

After `make verify` passes and devices are available, deployment tests are the
primary feedback mechanism for validating changes on real hardware.
The `remote_test.sh` workflow emits strict exit codes and an optional JSON
summary that agents MUST use to drive the edit-build-test loop.

### Exit Code Reference

| Exit code | Meaning | Agent action |
|-----------|---------|--------------|
| **0** | Run completed successfully | Proceed. Test passed. |
| **1** | Binary exited non-zero | Diagnose: read stdout/stderr for error messages. Usually a code bug. Follow Error Recovery Loop. |
| **124** | Timeout (binary did not finish) | Likely a hang or infinite loop. Check dmesg output for kernel messages. If dmesg shows ISP/VIF/sensor errors, the pipeline is stuck. Log in CRASH_LOG.md and reboot before retrying. |
| **2** | Device unresponsive after run | **Stop all testing.** Board needs a power cycle. Log in CRASH_LOG.md. Do NOT attempt further SSH commands — they will fail. Wait for human to power-cycle, then re-run from scratch with `--reboot-before-run`. |

### JSON Summary (--json-summary)

When `--json-summary` is passed, `remote_test.sh` emits a single JSON line
after all output. This is the preferred way for agents to consume results:

```json
{"status":"success","exit_code":0,"device_alive":true,"dmesg_hits":0,"duration_sec":12,"run_bin":"venc","soc_build":"star6e","host":"root@192.168.1.13"}
```

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `success`, `failed`, `timeout`, or `device_unresponsive` |
| `exit_code` | int | Raw exit code from the binary (or 124 for timeout) |
| `device_alive` | bool | SSH liveness check after run |
| `dmesg_hits` | int | Count of kernel log lines matching ISP/sensor/fault keywords |
| `duration_sec` | int | Wall-clock seconds the run took |
| `run_bin` | string | Which binary was executed |
| `soc_build` | string | Backend that was built and deployed |
| `host` | string | Target device SSH address |

### Quick-Deploy Modes

Both backends use dlopen for all MI vendor libs at runtime. Star6E runtime
libs live in `/usr/lib` on the target firmware. Maruko deployments transfer
the vendored MI SDK libs (from `libs/maruko/`) via `remote_test.sh`.
Use these flags to skip unnecessary work:

| Flag | What it skips | When to use |
|------|---------------|-------------|
| `--skip-build` | Build step | Binary already built locally (e.g., after `make build`) |
| `--skip-deploy` | Binary transfer | Re-running same binary with different args |

Typical fast iteration cycle:

```bash
# First run: full build + deploy
make remote-test ARGS='--json-summary --host root@192.168.1.13 -- --sensor-index 0 -f 30'

# After editing venc source: rebuild locally, deploy the binary
make build SOC_BUILD=star6e
make remote-test ARGS='--json-summary --skip-build --host root@192.168.1.13 -- --sensor-index 0 -f 30'

# Same binary, different args: skip everything, just re-run
make remote-test ARGS='--json-summary --skip-build --skip-deploy --host root@192.168.1.13 -- --sensor-index 0 -f 60'
```

### Interpreting dmesg Hits

- **0 hits**: Clean run. No kernel-level issues.
- **1-5 hits with keywords like `snr`, `vif`, `isp`**: Informational. Common
  during sensor init. Not a failure unless the run also failed.
- **Hits with `fault`, `oops`, `panic`, `segmentation`**: Kernel-level crash.
  The binary (or a driver it triggered) caused a serious error. Always log in
  CRASH_LOG.md. Reboot before next test.
- **Hits with `watchdog`, `timeout`**: Hardware watchdog or driver timeout.
  May indicate a stuck pipeline. Reboot recommended.

### Agent Decision Flow

```
Run deployment test with --json-summary
  ├── status=success, dmesg_hits=0  → PASS, continue
  ├── status=success, dmesg_hits>0  → PASS with warnings, check dmesg keywords
  ├── status=failed                 → Read stdout/stderr, enter Error Recovery Loop
  ├── status=timeout                → Check dmesg, log in CRASH_LOG.md, reboot
  └── status=device_unresponsive    → STOP. Log CRASH_LOG.md. Wait for power cycle.
```

## JSON Config Deploy & Test (Preferred)

The preferred method for interactive testing uses the device's production
JSON config (`/etc/venc.json`) and the HTTP API. This tests the actual
runtime path including config parsing, pipeline init, and API controls.

**Important:** The venc binary always loads `/etc/venc.json` — this path
is hardcoded. There is no `-c` flag to override it at runtime.

**Important:** Always use the `json_cli` binary on the target for JSON config
edits. Do NOT use `sed` — it is unreliable on nested JSON (wrong key matches,
encoding issues). `json_cli` is the only safe tool for modifying
`/etc/venc.json` on the device.

```bash
# Set a field (dot-path, updates in-place)
ssh root@<HOST> "json_cli -s .audio.codec '\"opus\"' -i /etc/venc.json"
ssh root@<HOST> "json_cli -s .audio.enabled true -i /etc/venc.json"
ssh root@<HOST> "json_cli -s .audio.sampleRate 48000 -i /etc/venc.json"

# Read a field
ssh root@<HOST> "json_cli -g .audio.codec --raw -i /etc/venc.json"

# Verify the full audio section after edits
ssh root@<HOST> "json_cli -g .audio -i /etc/venc.json"
```

Path syntax: `.field`, `.nested.field`, `.array[0]`. String values must be
quoted JSON strings: `'"hello"'` (shell single-quotes around JSON double-quotes).
Booleans and numbers are bare: `true`, `false`, `42`.

### Quick cycle

For the current Star6E bench (`root@192.168.1.13`), prefer the helper script:

```bash
scripts/star6e_direct_deploy.sh cycle
```

It wraps the production `/etc/venc.json` flow: config backup, `/usr/bin/venc`
deploy, daemon start, HTTP readiness wait, endpoint checks, and log capture.

```bash
# 1. Build
make build SOC_BUILD=star6e

# 2. Stop running instance
ssh root@<HOST> "killall venc; sleep 2"

# 3. Deploy binary
scp -O out/star6e/venc root@<HOST>:/usr/bin/venc

# 4. (Optional) Modify config — always use json_cli, never sed
ssh root@<HOST> "json_cli -s .legacyAe false -i /etc/venc.json"
ssh root@<HOST> "json_cli -s .system.verbose true -i /etc/venc.json"

# 5. Start venc as daemon with log capture
ssh root@<HOST> "nohup venc > /tmp/venc.log 2>&1 &"

# 6. Wait for pipeline init (~10s), then query
sleep 10
ssh root@<HOST> "wget -q -O- http://127.0.0.1/api/v1/ae"

# 7. Check startup log
ssh root@<HOST> "cat /tmp/venc.log"

# 8. Live API controls
ssh root@<HOST> "wget -q -O- 'http://127.0.0.1/api/v1/set?isp.gainMax=10000'"
ssh root@<HOST> "wget -q -O- 'http://127.0.0.1/api/v1/set?isp.exposure=3'"

# 9. Verify via AE diagnostics
ssh root@<HOST> "wget -q -O- http://127.0.0.1/api/v1/ae"

# 10. Check for specific log patterns
ssh root@<HOST> "grep 'limits updated' /tmp/venc.log"

# 11. Cleanup — restore config and stop
ssh root@<HOST> "json_cli -s .legacyAe true -i /etc/venc.json"
ssh root@<HOST> "json_cli -s .system.verbose false -i /etc/venc.json"
ssh root@<HOST> "killall venc"
```

### Key endpoints for verification

| Endpoint | Purpose |
|----------|---------|
| `/api/v1/ae` | AE diagnostics: ISP state, exposure limits, sensor plane, stability |
| `/api/v1/awb` | AWB diagnostics: gain values, color temp, stabilizer state |
| `/api/v1/config` | Full active runtime config |
| `/api/v1/set?<field>=<value>` | Live config change (see HTTP_API_CONTRACT.md) |
| `/metrics/isp` | Compact ISP metrics (Prometheus format) |

### Why this method over remote-test

- Tests the **production config path** (`/etc/venc.json` parsing, not CLI args)
- Validates **HTTP API** integration end-to-end
- Supports **live parameter changes** without restart
- Log persists in `/tmp/venc.log` for post-mortem analysis
- Binary stays deployed in `/usr/bin/venc` for manual re-runs

The `make remote-test` workflow (below) is still useful for automated sensor
mode sweeps and CI, but JSON config deploy-and-test is the preferred method
for interactive development and feature validation.

## Error Recovery Loop

When any build or verification step fails, follow this structured cycle.
Do not abandon failed work or start over from scratch — treat debugging as
part of normal execution.

```
Observe → Diagnose → Repair → Re-verify → Document
```

### 1. Observe

Read the **full** error output. Do not skim. Key patterns:

| Error type | What to look for |
|---|---|
| Compiler error | File, line number, error message. Read the indicated source line. |
| Linker error | `undefined reference to` → missing source file in Makefile `SRC`, or missing `-l` flag. `multiple definition` → duplicate symbol across translation units. |
| Binary missing | `make verify` says `FAIL ... not found` → the build target did not produce output. Check the compiler invocation succeeded. |
| Runtime crash | `dmesg` kernel messages, segfault address, signal number. Cross-reference with `documentation/CRASH_LOG.md`. |
| Timeout | `remote_test.sh` timed out → possible board hang. Check SSH liveness before retrying. |

### 2. Diagnose

- Identify the **single root cause**. Most build failures have one root cause
  that cascades into multiple errors. Fix the first error; the rest often
  disappear.
- If the error is in SDK headers (`sdk/`), you cannot modify those files.
  Adjust your code to match the SDK's expectations.
- If you are unsure, re-read the relevant documentation listed in
  **Key Documentation** before guessing.

### 3. Repair

- Make the **minimal** fix for the identified root cause.
- Do not refactor surrounding code while fixing a build error.

### 4. Re-verify

- Run `make lint` for a quick syntax/warning check.
- Then re-run the full command that failed (`make build`, `make verify`, etc.).
- If it fails again, return to step 1. Do not skip ahead.

### 5. Document

- If the failure revealed a non-obvious constraint (e.g., an SDK quirk,
  alignment requirement, or initialization order dependency), note it in the
  relevant documentation file or add a brief code comment.
- If a runtime crash or hang occurred, log it in `documentation/CRASH_LOG.md`.

### Deployment Test Recovery

Deployment test failures follow the same Observe → Diagnose → Repair →
Re-verify → Document cycle, but with hardware-specific recovery steps:

| Failure | Recovery |
|---------|----------|
| **exit 1** (binary error) | Read error output. Fix code. Rebuild with `make build`. Re-deploy (default, or use `--skip-build` if already rebuilt). Re-run. |
| **exit 124** (timeout) | Collect dmesg output from the test run. Check if device is still reachable. If yes, reboot with `--reboot-before-run` and re-test. If no, treat as device_unresponsive. |
| **exit 2** (device unresponsive) | **Do not retry.** Log the incident in CRASH_LOG.md with exact command, circumstances, and dmesg (if available). Inform the user that a power cycle is needed. After power cycle, re-establish connectivity, then re-deploy from scratch (full deploy, not binary-only — `/tmp` is wiped on reboot). |
| **dmesg panic/fault** | Even if the run "succeeded", a kernel fault means the driver state is corrupted. Reboot before any further testing. Log the fault. |

**Critical rule:** After any device unresponsive event, the agent MUST NOT
attempt further SSH commands to that host until a human confirms the device
is back. Repeated SSH attempts to a hung board waste time and can mask the
root cause in logs.

## Long-Session Guidance

For multi-step tasks that span many changes:

### Progress Checkpoints

- Use todo/task tracking to maintain a list of completed and remaining steps.
  This serves as externalized state that survives context boundaries.
- After completing each milestone, verify the build still passes before
  moving on. A passing build is your checkpoint; never stack multiple
  unverified changes.

### Decision Stability

- Once the plan (Phase 1) is approved, follow it. Do not switch approaches
  mid-implementation unless the current approach is proven unworkable by a
  concrete failure.
- If you discover during implementation that the plan needs adjustment, stop
  and get human approval for the revised plan before continuing.

### Scope Control

- Each change should be the smallest unit that can be independently verified.
  Prefer multiple small, verified commits over one large commit.
- If a task has independent sub-parts (e.g., Star6E changes + Maruko changes),
  implement and verify each sub-part separately.

## Operational Defaults

### Deployment Targets

Each row is a testable SoC/sensor combination. Fill in the **Host** column
with the device IP to include it in verification deployments. Leave it blank
to skip that combination.

| Backend | SoC | Sensor | Sensor Index | Host | ISP Bin | Extra Args |
|---------|-----|--------|--------------|------|---------|------------|
| Star6E | ssc30kq | imx415 | 0 | | | |
| Star6E | ssc30kq | imx335 | 0 | | | |
| Star6E | ssc338q | imx415 | 0 | | `/etc/sensors/imx415_greg_fpvVII-gpt200.bin` | `--verbose` |
| Star6E | ssc338q | imx335 | 0 | 192.168.1.13 | `/etc/sensors/imx335_greg_fpvVII-gpt200.bin` | `--verbose` |
| Maruko | ssc378qe | imx415 | 0 | 192.168.2.12 | `/etc/sensors/imx415.bin` | |
| Maruko | ssc378qe | imx335 | 0 | | `/etc/sensors/imx335.bin` | |

- Stream destination host for 192.168.1.X network: `192.168.1.2`
- Stream destination host for 192.168.2.X network: `192.168.2.2`
- **Extra Args** are passed to the binary in deployment test invocations.

## Key Documentation

Read these before working on related areas:

| Area | Document |
|------|----------|
| Sensor unlock / high-FPS | `documentation/SENSOR_UNLOCK_IMX415_IMX335.md` |
| Current priorities | `documentation/CURRENT_STATUS_AND_NEXT_STEPS.md` |
| Implementation timeline | `documentation/IMPLEMENTATION_PHASES.md` |
| Star6E direct deploy helper | `scripts/star6e_direct_deploy.sh` |
| Remote testing | `documentation/REMOTE_TEST_WORKFLOW.md` |
| HTTP API contract | `documentation/HTTP_API_CONTRACT.md` |
| Precrop / aspect ratio | `documentation/PRECROP_ASPECT_RATIO.md` |
| SigmaStar SDK reference | `documentation/PROC_MI_MODULES_REFERENCE.md` |
| Crash log / incidents | `documentation/CRASH_LOG.md` |
| Target agent design (future) | `documentation/TARGET_AGENT_ARCHITECTURE.md` |
| SD card recording | `documentation/SD_CARD_RECORDING.md` |
| Coding conventions | `documentation/CONVENTIONS.md` |
| Code structure prestudy | `documentation/CODE_STRUCTURE_PRESTUDY.md` |
| Refactoring plan | `documentation/REFACTORING_PLAN.md` |

## Config / WebUI / API Sync Rules

The config system spans three layers that MUST stay in sync:

1. **C struct + parser** (`include/venc_config.h`, `src/venc_config.c`) — defines
   all fields, defaults, JSON key names for parsing and serialization.
2. **API field table + alias table** (`src/venc_api.c`) — `g_fields[]` maps
   `section.snake_case` names to struct offsets; `g_aliases[]` maps camelCase
   webui keys to snake_case API keys.
3. **WebUI dashboard** — authored as plain HTML/JS in `web/dashboard.html`
   and embedded as a gzip blob in `src/venc_webui.c`.  `SECTIONS[]` lists
   every field the UI exposes, using camelCase keys that must match either the
   config JSON key names (for value lookup) or the alias table (for API set calls).
   Regenerate the embedded blob deterministically with `make webui`
   (or `python3 tools/build_webui.py`).  CI runs `make webui-check` via
   `make verify` to catch drift between source and embedded blob.
4. **Default config file** (`config/venc.default.json`) — reference config
   showing all available options with sensible defaults.

When adding or removing a config field, update ALL FOUR locations in the same PR:

- Add the field to `VencConfig` struct and set its default in `venc_config_defaults()`
- Add parsing in the `load_<section>()` function and serialization in `venc_config_to_json()`
- Add a `FIELD()` entry in `g_fields[]` and a camelCase alias in `g_aliases[]` if needed
- Add the key to the appropriate `SECTIONS[]` entry in `web/dashboard.html`
- Add the key with its default value to `config/venc.default.json`
- Regenerate the embedded gzip: `make webui` (runs `tools/build_webui.py`)

Key naming conventions:
- C struct fields: `snake_case` (e.g., `sample_rate_hz`)
- API field names: `section.snake_case` (e.g., `imu.sample_rate_hz`)
- Config JSON keys: `camelCase` (e.g., `sampleRateHz`)
- WebUI SECTIONS keys: must match config JSON keys exactly
- Alias table: maps config JSON camelCase → API snake_case

## Versioning Policy

- SemVer tracked in `VERSION` file.
- `HISTORY.md` is the canonical changelog.
- **One version bump per PR.** A PR contains exactly one `VERSION` bump and
  one matching `HISTORY.md` entry. If intermediate commits within the branch
  touch version/changelog, squash them into a single entry before merging.
- Before opening any PR:
  - Bump `VERSION`.
  - Add a matching `HISTORY.md` entry.
- If a PR changes HTTP API behavior:
  - Update `documentation/HTTP_API_CONTRACT.md` in the same PR.
  - Keep endpoints lean/focused, JSON payloads simple + descriptive.

## Implementation Policy (Backend Split)

- New SigmaStar SDK features: implement on Star6E first, validate, then port
  to Maruko as a follow-up.
- Shared features (JSON config, HTTP API): implement for both backends in the
  same phase, but Maruko may ship with stubs until Star6E is validated.

## Coding Conventions

Full conventions are in `documentation/CONVENTIONS.md` (the authoritative
reference). Key rules summarized here:

- C99, no C++ features.
- Tabs for indentation in C source.
- `snake_case` for functions, `PascalCase` for types, `SCREAMING_SNAKE` for
  constants. Module prefix on public functions (e.g., `rtp_send_packet`).
- No `__double_underscore__` reserved identifiers. No `camelCase` in new code.
- No global mutable state beyond what already exists.
- Error messages go to stderr. Informational output to stdout.
- Keep functions under ~80 lines. Extract helpers when logic gets dense.
- No unused includes, no unused variables, no dead code.
- `const` on pointer parameters that are not modified.
- One concern per file. Every `.c` has a matching `.h`.
- Headers include only what they need — no mega-headers.

## Build Commands Reference

| Command | Description |
|---------|-------------|
| `make build` | Build Star6E backend (default) → `out/star6e/venc` |
| `make build SOC_BUILD=maruko` | Build Maruko backend → `out/maruko/venc` |
| `make lint` | Fast warning check (compile with `-Wall -Werror`, no link) |
| `make lint SOC_BUILD=maruko` | Lint Maruko backend |
| `make stage` | Build + stage runtime bundle in `out/<soc>/` |
| `make clean` | Clean build outputs |
| `make verify` | Build both backends + verify binaries (Maruko first, Star6E last) |
| `make pre-pr` | Full pre-PR checklist |
| `make remote-test ARGS='...'` | Run bounded remote CLI/test-binary workflow |
| `scripts/star6e_direct_deploy.sh cycle` | Preferred Star6E `venc` deploy + HTTP smoke test |

### Output Layout

Each backend builds to its own directory under `out/`:

```
out/star6e/venc                  ← Star6E binary
out/maruko/venc                  ← Maruko binary
```

Both backends can coexist — `make verify` builds both without cleaning
between them.  `make build` (default) always produces `out/star6e/venc`.

## Mistakes to Avoid

- Do NOT modify `sdk/` headers. They are vendor-provided and read-only.
- Do NOT add runtime SoC autodetection. Backend is selected at build time.
- Do NOT use floating-point math in precrop or alignment calculations.
- Do NOT commit toolchain/ or build output (out/).
- Do NOT add dependencies beyond the SigmaStar SDK and standard C library.
- Do NOT use `--no-verify` or skip git hooks.
- Do NOT open a PR without running `make verify` first.
- Do NOT stack multiple unverified changes. Build after each logical change.
- Do NOT ignore the first compiler error and try to fix later errors instead.
  Fix errors in order; earlier errors often cause later ones.
- Do NOT switch implementation approach mid-task without re-planning. If the
  current approach fails, diagnose why before changing course.
- Do NOT add or remove a config field without updating all four layers: C struct/parser,
  API field+alias tables, WebUI SECTIONS, and default config file. See
  **Config / WebUI / API Sync Rules** above.

## Codex delegation

OpenAI Codex is available via the `codex@openai-codex` Claude Code plugin for
verification and token offload. Use `/codex:review` or
`/codex:adversarial-review` for second-opinion passes, and `/codex:rescue`
(optionally `--background`) to delegate heavy reads, full-repo greps, or
self-contained investigations. The workflow spec lives in the coordination
repo at `docs/codex-workflow.md`.
