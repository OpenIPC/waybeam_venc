# Coding Conventions & Style Guide
<!-- version: 1.0.0 -->

This document is the authoritative reference for coding conventions in the
venc project. All new code and refactored code MUST follow these rules.
Existing code is grandfathered until actively modified — once you touch a
function, bring it into compliance.

Referenced by: `AGENTS.md`, `CLAUDE.md`

---

## 1. Language & Standard

- **C99** (`-std=c99`). No C++ features, no GNU extensions unless required
  by the SDK.
- No floating-point math in alignment, crop, or pipeline calculations.
- Standard library + SigmaStar SDK + POSIX are the only allowed dependencies.

## 2. Formatting

| Rule | Convention |
|------|-----------|
| Indentation | Tabs (1 tab = display-width 4, but tabs, not spaces) |
| Line length | Soft limit 100 columns, hard limit 120 |
| Braces | K&R style — opening brace on same line, except for function definitions |
| Pointer star | Attached to variable: `int *ptr`, not `int* ptr` |
| Trailing whitespace | None |
| Final newline | Every file ends with exactly one newline |

```c
/* Function definition: opening brace on its own line */
static int pipeline_init(const VencConfig *cfg, PipelineState *ps)
{
	if (cfg->width == 0) {
		fprintf(stderr, "invalid width\n");
		return -1;
	}
	return 0;
}
```

## 3. Naming Conventions

### 3.1 Functions

| Scope | Convention | Example |
|-------|-----------|---------|
| Public (in a header) | `module_verb_noun` snake_case | `rtp_send_packet()`, `sensor_select()` |
| Static (file-local) | `verb_noun` snake_case | `fill_h26x_attr()`, `strip_start_code()` |
| Backend-specific public | `backend_module_verb` | `star6e_pipeline_init()` |

- Every public function MUST have a module prefix matching its header file.
- No `camelCase` for new functions. When refactoring legacy code, rename to
  snake_case.

### 3.2 Types

| Kind | Convention | Example |
|------|-----------|---------|
| Structs / typedefs | `PascalCase` | `VencConfig`, `SensorStrategy`, `RtpState` |
| Enums | `PascalCase` type, `SCREAMING_SNAKE` values | `StreamMode`, `STREAM_MODE_RTP` |
| SDK/vendor types | Keep vendor naming | `MI_S32`, `MI_SNR_PAD_ID_e` |

### 3.3 Variables

| Scope | Convention | Example |
|-------|-----------|---------|
| Local variables | `snake_case` | `frame_count`, `pad_id` |
| File-scope globals | `g_` prefix + `snake_case` | `g_running`, `g_pipeline_state` |
| Constants / macros | `SCREAMING_SNAKE` | `RTP_DEFAULT_PAYLOAD`, `MAX_CHANNELS` |

### 3.4 Reserved Identifiers

- **NEVER** use double-underscore prefixes/infixes (`__foo__`, `__bar`).
  These are reserved by the C standard.
- **NEVER** use identifiers starting with `_` followed by an uppercase letter.

## 4. File & Module Organization

### 4.1 One Concern Per File

Each `.c` file implements exactly one module. Each module has a matching `.h`
header exposing only its public interface.

```
src/rtp.c          → include/rtp.h           (RTP packetization)
src/pipeline.c     → include/pipeline.h      (shared pipeline orchestration)
src/audio.c        → include/audio.h         (audio capture + encoding)
src/venc_config.c  → include/venc_config.h   (JSON config)
```

### 4.2 Header Discipline

- Every header uses an include guard: `#ifndef MODULE_H` / `#define MODULE_H`.
- Headers include **only what they need** for their own declarations.
- No "mega-headers" that transitively include the entire SDK.
- Forward-declare structs in headers when a pointer is sufficient; include
  the full definition only in the `.c` file.

### 4.3 Include Order

Within a `.c` file, order includes as:

```c
#include "mymodule.h"        /* 1. Own header (verifies self-containment) */

#include "other_project.h"   /* 2. Other project headers */

#include <stdio.h>           /* 3. Standard library */
#include <stdlib.h>

#include "star6e.h"           /* 4. SDK / vendor headers */
```

Separate each group with a blank line.

## 5. Function Design

- **Max ~80 lines** per function. If longer, extract a helper.
- **Single return point** preferred for functions with cleanup/teardown.
  Multiple early returns are fine for guard clauses at the top.
- **No side effects in macros.** Use `static inline` functions instead.
- **`const` correctness**: mark pointer parameters `const` when the function
  does not modify the pointed-to data.
- **Error returns**: `0` for success, `-1` (or negative errno) for failure.
  Document the contract in the header comment.

## 6. Error Handling & Logging

- Error messages → `stderr` via `fprintf(stderr, ...)`.
- Informational output → `stdout`.
- Format: `"[module] message: %s\n", strerror(errno)` when an errno-setting
  call fails.
- No `printf` debugging left in committed code. Use `--verbose` / a verbose
  flag for optional diagnostics.

## 7. Backend Abstraction

### 7.1 Backend Registration (Target State)

Backends implement a set of operations and register via a function-pointer
struct. Shared orchestration code calls through this interface.

```c
/* target pattern — do not implement until refactoring phase */
typedef struct {
    const char *name;
    int (*init)(const VencConfig *cfg, PipelineState *ps);
    int (*start_stream)(PipelineState *ps);
    void (*stop)(PipelineState *ps);
} BackendOps;
```

### 7.2 Shared vs. Backend-Specific Code

| Code belongs in... | When... |
|--------------------|---------|
| Shared module (`src/rtp.c`, `src/pipeline_common.c`) | Logic is identical across backends |
| Backend file (`src/backend_star6e.c`) | Logic uses backend-specific SDK calls or types |

**Rule of thumb:** if you are copy-pasting code between backends, it belongs
in a shared module.

## 8. Build System

- Source files listed explicitly in `Makefile` per-backend — no wildcards.
- Shared source files compiled into both backends.
- Build must pass with `-Wall -Wextra -Werror` (modulo SDK header warnings
  suppressed via `-Wno-unused-parameter -Wno-old-style-declaration`).
- `make lint` after every logical change.

## 9. Comments & Documentation

- **Do not over-comment.** Code should be self-documenting through clear
  naming and small functions.
- **Do comment:** non-obvious workarounds, SDK quirks, hardware constraints,
  "why" (not "what").
- Header files: brief `/** one-liner */` above each public function.
- No `// removed:` or `// TODO: maybe` comments. Either do it or don't.
- Section separators in long files are fine:
  ```c
  /* ── RTP packetization ──────────────────────────────────── */
  ```

## 10. Version Control

- Atomic commits: one logical change per commit.
- Commit message: imperative mood, ≤72 chars first line.
- No committed build artifacts, toolchains, or `.env` files.
- `VERSION` + `HISTORY.md` updated per the versioning policy in `AGENTS.md`.

---

## Changelog

| Version | Date | Change |
|---------|------|--------|
| 1.0.0 | 2026-03-11 | Initial conventions document based on code structure prestudy |
