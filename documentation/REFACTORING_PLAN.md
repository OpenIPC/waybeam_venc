# Code Structure Refactoring Plan
<!-- version: 1.1.0 -->

Based on: `documentation/CODE_STRUCTURE_PRESTUDY.md`
Conventions: `documentation/CONVENTIONS.md`
Workflow: `AGENTS.md`

---

## Scope & Constraints

- This plan targets `star6e-standalone/` only.
- All incoming PRs are already merged. The baseline for this plan is the
  current `master` branch after the merge set completed.
- Refactoring work happens on a dedicated refactor branch cut from `master`.
  `master` remains the stable release branch while the refactor is in flight.
- **Star6E is the reference implementation.** Its current behavior and feature
  set define the refactor target:
  - current CLI behavior
  - current HTTP API behavior
  - current RTP/compact/SHM streaming behavior
  - current WFB-related Star6E packetization behavior
- Maruko is expected to share all truly common code, but it is not the source
  of truth for behavior during this refactor. Where Star6E and Maruko differ,
  preserve Star6E behavior first and use Maruko adapters or temporary
  backend-local code until convergence is safe.
- Behavioral changes are out of scope. The refactor must be transparent:
  same binary behavior, same CLI interface, same HTTP API, same streaming
  output.
- Every gate must leave the tree buildable and releasable. Do not stack
  multiple unverified refactor steps.

---

## Gate Structure

Each gate follows the standard project pipeline:
**Spec -> Draft -> Simplify -> Verify**

A gate is complete when:
1. `make verify` passes.
2. `make -C star6e-standalone test` passes.
3. Deployment tests pass for every populated row in
   `AGENTS.md -> Operational Defaults -> Deployment Targets`.
   At the time of this plan update, that means the populated Star6E rows are
   in scope; blank Maruko rows are skipped unless the table is updated.
   Until Star6E CLI-driven deployment control is refactored, replay the
   exact config-driven reconfirmation procedure recorded in
   `documentation/REFACTOR_BASELINE_2026-03-12.md`.
4. Code review confirms no intended Star6E behavior change.

If a gate exposes a Star6E vs Maruko mismatch, do not force a premature
unification. Keep the Star6E path authoritative and document why the Maruko
path remains backend-local.

---

## Gate 0: Baseline Capture On Refactor Branch

**Goal:** Start from a clean, documented baseline now that the merge backlog is
gone.

**Entry criteria:**
- Working tree clean on `master`.
- Refactor branch created from current `master`.
- `make verify` passes.
- `make -C star6e-standalone test` passes.

**Actions:**
1. Create the dedicated refactor branch from `master`.
2. Tag the baseline commit: `git tag pre-refactor-baseline`.
3. Run:
   - `make verify`
   - `make -C star6e-standalone test`
4. Run deployment tests for every populated host row in `AGENTS.md`:
   - first `--list-sensor-modes`
   - then every reported sensor mode at its max FPS
5. Record the baseline commit, test results, and any device-specific caveats.

**Exit criteria:**
- Tagged baseline exists.
- Baseline report exists with build, host-test, and deployment results.

**Verification:**
```bash
make verify
make -C star6e-standalone test
```

---

## Gate 1: Remove Legacy Header Coupling

**Goal:** Eliminate `include/main.h` and `src/shared.c`, remove reserved
identifiers, and establish clean module boundaries without changing backend
lifecycle flow.

**Scope:**
- Remove `include/main.h` as the legacy mega-header.
- Remove `src/shared.c`.
- Move still-needed declarations into focused headers or modules:
  - CLI help into `cli.c` / `cli.h` or equivalent
  - RTP packet structures/helpers into dedicated RTP headers if still needed
  - small utility macros/functions into dedicated utility headers
- Replace `main.h` includes in:
  - `src/backend_star6e.c`
  - `src/backend_maruko.c`
  - `src/snr_toggle_test.c`
  - `src/snr_sequence_probe.c`
  - any other remaining users
- Update `star6e-standalone/Makefile` source and dependency lists.
- Remove all `__double_underscore__` identifiers and legacy parser macros.
- Rename touched legacy `camelCase` project functions to `snake_case`.
- Do **not** change pipeline orchestration in this gate.

**Design decisions:**
- Prefer small, self-contained headers over a replacement mega-header.
- Remove dead code rather than preserving it behind compatibility wrappers.
- Treat backend compile independence as part of the goal: each file should
  include only what it directly needs.

**Risk:** Low to medium. The main risk is accidental include fallout in test
helpers and the standalone `Makefile`.

**Verification:**
```bash
make lint
make lint SOC_BUILD=maruko
make verify
make -C star6e-standalone test
```

---

## Gate 2: Extract Shared Helpers From The Star6E Template

**Goal:** Deduplicate code where Star6E and Maruko are genuinely common, while
keeping Star6E-specific behavior intact.

**Principle:** Extract from Star6E outward. A helper becomes shared only when
Star6E can keep its current behavior exactly and Maruko can adopt it via a
thin adapter or compatible type boundary.

### Gate 2A: Low-Risk Shared Helpers

**Candidate modules:**

| Module | Extracts from | Contents |
|--------|---------------|----------|
| `file_util.c` / `file_util.h` | Both backends | `validate_regular_file` and similar pure file checks |
| `h26x_util.c` / `h26x_util.h` | Both backends | start-code stripping, NAL type helpers, small codec parsing helpers |
| `sdk_quiet.c` / `sdk_quiet.h` | Both backends + `sensor_select.c` | SDK stdout suppression where signatures match cleanly |

### Gate 2B: Shared Configuration Helpers

**Candidate modules:**

| Module | Extracts from | Contents |
|--------|---------------|----------|
| `codec_config.c` / `codec_config.h` | Both backends | codec/RC resolution helpers where behavior matches |
| `pipeline_common.c` / `pipeline_common.h` | Both backends | small shared helpers that stay SDK-neutral or adapter-driven |

### Gate 2C: Packetizer Extraction Only Where Truly Common

**Candidate modules:**

| Module | Extracts from | Contents |
|--------|---------------|----------|
| `rtp_packetizer.c` / `rtp_packetizer.h` | Both backends | shared RTP primitives, H.264/H.265 FU helpers, RTP header emission helpers |

**Do not force into shared code in this gate unless proven safe:**
- Star6E SHM RTP-ring output support
- Star6E HEVC AP aggregation path
- Star6E packetizer statistics/debug behavior
- Star6E-specific adaptive payload policy details

Those remain backend-local until they can be shared without changing Star6E
behavior.

**Process per helper/module:**
1. Start from the Star6E implementation as the canonical version.
2. Diff the Maruko implementation against it.
3. Extract only the behaviorally common core.
4. Keep backend-specific wrappers for remaining SDK/type differences.
5. Run `make lint` after each logical extraction, then re-run full verify.

**Risk:** Medium. The old plan underestimated divergence here. Star6E has
grown beyond the prestudy snapshot, especially in RTP/SHM packetization.

**Verification:**
```bash
make verify
make -C star6e-standalone test
```

Deployment verification for this gate should prioritize Star6E transport
parity against the baseline, including direct UDP and current SHM/WFB-related
paths when those code paths are touched.

---

## Gate 3: Expand Backend Abstraction Around Shared Orchestration

**Goal:** Turn the existing backend selection shim in `include/backend.h` into
the real backend abstraction layer and move shared orchestration out of the
backend monoliths.

**Scope:**
- Expand `include/backend.h` with a real `BackendOps` interface.
- Introduce a shared pipeline orchestration module such as
  `src/pipeline.c` / `include/pipeline.h`.
- Move the shared lifecycle into that module:
  - init
  - configure
  - start stream
  - run loop
  - stop
  - teardown
- Keep backend files responsible only for hardware-divergent behavior.
- Preserve Star6E init order, teardown order, signal/reinit behavior, and
  streaming behavior exactly.
- Allow Maruko to retain thin adapters or temporary backend-local hooks while
  the abstraction settles.

**Depends on:** Gate 2. Do not build a vtable layer on top of still-duplicated
backend monoliths.

**Risk:** High. This is the most invasive gate because it touches lifecycle and
cleanup ordering.

**Verification:**
```bash
make verify
make -C star6e-standalone test
```

Deployment verification for this gate should use the full populated device
matrix from `AGENTS.md`, including all reported sensor modes at max FPS.

---

## Gate 4: Clean Up Constants, Types, And const-Correctness

**Goal:** Finish the lower-risk structural cleanup once the main module
boundaries are stable.

**Scope:**
- Move duplicated constants into shared headers where the ownership is now
  obvious.
- Move Maruko-specific binding types out of `backend_maruko.c` into dedicated
  headers or modules.
- Add `const` annotations to read-only parameters.
- Review duplicated enums and shared typedefs.
- Remove any temporary compatibility helpers introduced during earlier gates.

**Design decision:**
- Do this after the larger structural gates so the type cleanup follows the
  new module boundaries instead of being done twice.

**Risk:** Low.

**Verification:**
```bash
make verify
make -C star6e-standalone test
```

---

## Gate 5: Output / Transport Sink Abstraction

**Goal:** Replace inline transport branching with an explicit output-sink
interface once the backend and packetizer boundaries are stable.

**Scope:**
- Define an output sink interface such as:
  - `init`
  - `send_frame`
  - `teardown`
- Cover current transports explicitly:
  - UDP RTP
  - compact UDP
  - SHM RTP ring
- Preserve Star6E SHM semantics as the reference behavior.
- Keep future sinks such as RTSP/file output out of scope unless needed by the
  refactor itself.

**Depends on:** Gate 3.

**Risk:** Medium. This changes hot-path dispatch and must not regress packet
rate, latency, or WFB-related behavior on Star6E.

**Verification:**
```bash
make verify
make -C star6e-standalone test
```

Deployment verification for this gate should include packet-rate checks and
transport-specific smoke tests on Star6E.

---

## Decision Log

Decisions made during refactoring are recorded here to prevent revisiting.

| Date | Gate | Decision | Rationale |
|------|------|----------|-----------|
| 2026-03-11 | — | Initial plan drafted before merge backlog cleared | Based on prestudy snapshot |
| 2026-03-12 | — | Refactor work now starts from `master`, not `main` | Repository uses `master` and merge prerequisite is satisfied |
| 2026-03-12 | — | Star6E is the reference implementation for refactor decisions | It is the currently tested and feature-complete backend |
| 2026-03-12 | 2 | Shared extraction proceeds from Star6E outward | Avoids diluting current Star6E behavior to match lagging Maruko code |

---

## Changelog

| Version | Date | Change |
|---------|------|--------|
| 1.1.0 | 2026-03-12 | Updated plan for post-merge baseline, Star6E-first refactor strategy, corrected verification flow, and narrower shared-extraction gates |
| 1.0.0 | 2026-03-11 | Initial plan based on code structure prestudy |
