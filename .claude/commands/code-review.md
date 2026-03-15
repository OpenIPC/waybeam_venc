Adversarial Code Review

Review the staged or recent changes with a critical eye.

Pass 1 - Find issues:
1. Read every modified file (use `git diff` to identify them).
2. Check for:
   - Bugs, off-by-one errors, null dereferences, buffer overflows.
   - Style violations (see AGENTS.md coding conventions).
   - Missing error handling at system boundaries.
   - Unnecessary complexity or dead code.
   - SDK misuse (check `documentation/PROC_MI_MODULES_REFERENCE.md`).
3. List all findings with file:line references.

Pass 2 - Challenge findings:
4. Re-examine each finding. Is it a real problem or a false alarm?
5. Drop false positives. Keep only actionable issues.

Report: list confirmed issues, each with file:line and a fix suggestion.
