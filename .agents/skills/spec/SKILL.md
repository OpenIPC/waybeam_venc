---
name: spec
description: Plan a code change before implementation. Reads docs and source, produces a concise spec for human approval. Use for any non-trivial task.
---

# Spec (Phase 1)

Before writing any code, produce a plan for the requested change.

1. Read the relevant documentation in `documentation/`.
2. Read every source file you intend to modify.
3. Write a concise plan:
   - What changes and why.
   - Which files are affected.
   - Any risks or open questions.
4. Document key design decisions and their rationale. If there are multiple
   viable approaches, state which one you chose and why. This prevents
   oscillating between approaches during implementation.
5. Present the plan and wait for human approval before proceeding.

Do NOT skip to implementation. A good plan lets you one-shot the draft phase.
