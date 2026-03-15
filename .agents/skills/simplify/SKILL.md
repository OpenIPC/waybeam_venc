---
name: simplify
description: Review and simplify recent code changes. Remove unnecessary abstractions, dead code, and complexity while preserving behavior.
---

# Simplify (Phase 3)

Review the recent changes and simplify them.

1. Read every file that was modified in the current work.
2. For each change, ask:
   - Can this function be shorter or clearer?
   - Are there unnecessary abstractions, error paths, or comments?
   - Is there dead code, unused includes, or orphan declarations?
   - Does the architecture stay clean?
3. Make simplification edits directly.
4. Run `make lint` after each edit to catch regressions immediately.
5. After all simplifications, run `make verify` to confirm nothing broke.

Target: fewer lines, same behavior. Remove anything not strictly needed.
