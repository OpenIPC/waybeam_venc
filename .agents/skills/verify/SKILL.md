---
name: verify
description: Build both Star6E and Maruko backends and verify all expected binaries exist. Run this after any code change.
---

# Verify (Phase 4)

Run the full verification pipeline and report results.

1. Run `make lint` first for a fast warning check.
2. Run `make verify` to build both backends and check all expected binaries.
3. If any step fails, follow the Error Recovery Loop from AGENTS.md:
   - **Observe**: Read the full error output. Identify the error type
     (compiler, linker, binary missing, runtime crash, timeout).
   - **Diagnose**: Find the single root cause. Fix the first error; later
     errors often cascade from it.
   - **Repair**: Make the minimal fix for the root cause.
   - **Re-verify**: Run `make lint`, then re-run the failed command.
   - **Document**: Note any non-obvious constraint discovered.
4. Report the final status clearly.

Do NOT declare success until `make verify` passes cleanly.
