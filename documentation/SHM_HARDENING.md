Audit and harden the existing SHM SPSC zero-copy ring implementation (create/attach/destroy + inline write/read/read_wait paths) without redesigning it from scratch.

Goals:
- keep SHM
- keep strict SPSC semantics
- keep fixed-size slot design and zero-copy IPC behavior
- make failures degrade into packet drop, clean attach failure, slot reset, or explicit recovery instead of shared-structure corruption, overwrite, OOB access, deadlock, stale-slot misread, or crash

Review and improve the implementation in these areas:

1. Shared-memory ABI / header hardening
- Treat the SHM layout as a stable ABI.
- Add or verify:
  - magic
  - version
  - total mapped size
  - slot count
  - slot data capacity
  - optional init_complete flag
  - optional epoch / instance generation
- Add compile-time layout checks with `_Static_assert` where appropriate.
- Use fixed-width integer types only.
- Do not store pointers in SHM.

2. Safe size math
- Audit all size calculations for overflow:
  - slot stride
  - total mapping size
  - slot_count * stride
  - header + slot area
- Prefer checked `size_t` math or explicit overflow guards.
- Reject invalid sizes instead of truncating or wrapping.

3. Attach-time validation
- Harden attach logic to fail closed.
- Recompute expected slot stride and total size from header fields and require exact match.
- Validate:
  - magic
  - version
  - power-of-two slot count
  - nonzero slot_data_size
  - slot_data_size upper bound
  - total_size consistency
- Use `fstat()` and verify the real shm object size matches expected size.
- Revalidate the mapped header after `mmap()`.
- Reject stale, corrupt, inconsistent, or partially initialized mappings.

4. Lifecycle / restart safety
- Add or evaluate need for:
  - init_complete flag
  - epoch / instance id
  - clearer create/attach semantics for restart
- Ensure consumer cannot attach to or consume from partially initialized SHM.
- Consider producer restart, consumer restart, SHM recreation, and stale mappings.
- Prefer explicit refusal or recovery over undefined behavior.

5. Slot metadata hardening
- Audit all uses of slot length and any other slot metadata.
- On read, validate `slot->length <= slot_data_size` before any memcpy.
- Never trust SHM-derived metadata blindly.
- Invalid slot metadata must cause packet drop / recovery path, not unsafe copy or crash.

6. Full / empty behavior
- Verify full detection can never overwrite unread data.
- Verify empty detection can never misinterpret stale memory as new data.
- Keep bounded-ring semantics conservative and correct under wraparound.

7. Memory ordering / atomics
- Audit producer/consumer synchronization for correctness on ARM and x86, not just x86.
- Preserve or improve acquire/release ordering around publish/consume.
- Ensure there are no unsafe plain data races on synchronization-critical fields.
- Add comments explaining why each ordering is sufficient.

8. Futex / blocking read correctness
- Review the futex wait/wake design.
- Do not use futex directly on a 64-bit word if that is not a valid futex contract for the target platform.
- Consider introducing a dedicated 32-bit futex sequence / notify word instead of futex-waiting on `write_idx`.
- Keep blocking-read behavior correct under long runtimes, index growth, and wake races.
- Preserve correctness even if wakeups are missed or spurious.

9. Optional stronger slot validity model
- Evaluate whether per-slot state and/or per-slot generation counters should be added.
- If beneficial, use a minimal SPSC-safe scheme to detect stale slots after wraparound/restart/corruption.
- Keep it incremental; do not redesign into MPSC/MPMC.

10. Observability / counters
- Add lightweight counters for:
  - full drops
  - oversize drops
  - invalid slot length / metadata
  - attach validation failures
  - recovery/resync events
  - wake/wait anomalies if useful
- Add a compact debug dump/helper for header state and indices.

11. Debug hardening
- In debug/hardened builds, consider:
  - canaries
  - poison patterns
  - stronger assertions
  - extra validation
- Release builds should prefer drop/recover/fail-closed, not abort.

12. API / code hygiene
- Minimize direct raw SHM struct manipulation outside core helpers.
- Keep create/attach/read/write/recover logic centralized.
- Preserve current behavior and performance where safe.

13. Testing
- Add or improve tests for:
  - wraparound
  - ring full
  - ring empty
  - oversize writes
  - corrupt header fields
  - corrupt slot length
  - producer/consumer restart
  - stale mapping attach
  - blocking wake/wait edge cases
- Prove malformed metadata causes clean rejection, drop, or recovery rather than overwrite/OOB/crash.
14. Consider dual-ring architecture
- Evaluate whether the current hardening work should keep a single ring or split traffic into two separate SHM SPSC rings (for example audio ring + video ring).
- Prefer two separate rings if streams are logically independent, ordering between them does not matter, and isolation would improve robustness or simplify sizing/recovery.
- Compare single-ring vs dual-ring in terms of:
  - simplicity
  - fault isolation
  - drop behavior under burst load
  - audio starvation risk
  - recovery/reset behavior
  - implementation overhead
- If dual-ring is the better fit, keep the same hardening principles for each ring independently:
  - fixed-size bounded slots
  - strict SPSC ownership
  - validated attach/header logic
  - safe full/empty behavior
  - defensive metadata validation
  - clear recovery behavior
- Do not introduce router-thread complexity unless clearly justified.
- If keeping one ring, explicitly justify why that is better than two isolated rings for this workload.

Constraints:
- prefer minimal invasive changes
- preserve the current fast path where safe
- do not redesign from scratch
- do not generalize beyond SPSC
- keep fixed-size slots
- keep drop-on-full behavior unless there is a strong reason otherwise
- release behavior should prefer fail-closed, drop, or recover over abort

Deliverables:
- concise review of current risks
- concrete code patches
- short summary of which issues are fixed vs still optional
- brief remaining-risk checklist
