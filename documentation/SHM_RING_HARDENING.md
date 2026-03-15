# SHM Ring Hardening — Implementation Record

Completed: 2026-03-13

## Scope

Hardened `venc_ring.c`, `venc_ring.h`, `shm-input.patch`, tests, and tools.
No venc plumbing changes — `backend_star6e.c`, `main.c`, `send_rtp_packet`
are untouched. Both producer (venc) and consumer (wfb_tx) pick up fixes
via the shared header's inline functions at compile time.

## Risk Assessment (pre-hardening)

| # | Area | Risk | Resolution |
|---|------|------|------------|
| 1 | ABI header | Medium | Added `init_complete`, `epoch`, `futex_seq`; bumped to V2 |
| 2 | Size math | Medium | Overflow-checked `calc_total_size` (uint64_t intermediate) |
| 3 | Attach validation | High | fstat() size check, recomputed total, post-mmap re-validation |
| 4 | Lifecycle/restart | Medium | `init_complete` cleared on destroy (visible through shared pages); monotonic epoch |
| 5 | Slot metadata | **Critical** | Slot length validated in `venc_ring_read`; corrupt slots skipped |
| 6 | Full/empty | Low | Correct (unchanged) |
| 7 | Atomics/ordering | Low | Correct (unchanged) |
| 8 | Futex | **Critical** | Dedicated 32-bit `futex_seq` replaces futex-on-uint64_t `write_idx` |
| 9 | Per-slot state | Low | Not needed for strict SPSC with validated length |
| 10 | Counters | Medium | Per-handle `venc_ring_stats_t`: writes, reads, full/oversize/bad drops |
| 11 | Debug | Low | `_Static_assert` on header size and alignment |
| 12 | API hygiene | OK | Clean (unchanged) |
| 13 | Tests | Medium | 7 new hardening tests added (329 total) |
| 14 | Dual-ring | OK | Single ring for video, audio over UDP (unchanged) |

## Changes Implemented

### A. venc_ring.h — V2 header struct + inline paths

- `epoch` (uint32_t): monotonic clock ms, unique per create call
- `init_complete` (uint32_t): set last by creator (RELEASE), checked by consumer
- `futex_seq` (uint32_t): dedicated 32-bit word for futex wake/wait
- `venc_ring_stats_t`: per-handle counters (local, not in SHM)
- Slot length validation in `venc_ring_read`: reject `len > slot_data_size`
- uint32_t arithmetic for `hdr_len + payload_len` (prevents uint16_t overflow)
- `_Static_assert` on header size (192 bytes) and alignment (64 bytes)
- `VENC_RING_VERSION` bumped to 2

### B. venc_ring.c — create/attach/destroy

- `calc_total_size`: overflow-checked via uint64_t intermediate
- `venc_ring_create`: monotonic epoch, `init_complete` set last with RELEASE
- `venc_ring_destroy`: clears `init_complete = 0` (RELEASE) before munmap
  when is_owner — consumer's MAP_SHARED mmap sees the invalidation
- `venc_ring_attach`: fstat() size check, recomputed total, slot_data_size
  upper bound (≤ 65535), init_complete check, post-mmap header re-validation

### C. wfb_ng patch (shm-input.patch)

- Producer-restart detection via `init_complete` (not epoch) — works because
  both sides share the same underlying pages via MAP_SHARED
- Epoch logged on initial attach for diagnostics
- On `init_complete != 1`: detach, try re-attach, skip stale packets
- futex wait/wake uses `futex_seq` (via updated inline functions)

### D. Tests (test_venc_ring.c) — 329 total assertions

New tests:
1. `test_corrupt_header` — corrupt magic/version/total_size/slot_count → attach fails
2. `test_corrupt_slot_length` — slot->length > slot_data_size → read drops, advances
3. `test_overflow_create` — 1M slots × 65535 bytes → create returns NULL
4. `test_init_complete` — clear init_complete → attach fails; restore → succeeds
5. `test_stats_counters` — oversize_drops, full_drops, writes, reads increment
6. `test_write_u16_overflow` — uint32_t arithmetic prevents uint16_t wrap
7. `test_destroy_clears_init` — consumer sees init_complete=0 after producer destroy

### E. Tools

- `shm_ring_stats.c`: prints epoch, init_complete, futex_seq, version
- `shm_consumer_test.c`: prints epoch and version on attach

## Files Modified

| File | Change |
|------|--------|
| `star6e-standalone/include/venc_ring.h` | V2 header, inline fixes, counters |
| `star6e-standalone/src/venc_ring.c` | Overflow checks, attach hardening, destroy invalidation, monotonic epoch |
| `star6e-standalone/wfb/shm-input.patch` | init_complete detection, epoch logging, V2 ABI |
| `star6e-standalone/tests/test_venc_ring.c` | 7 new hardening tests |
| `star6e-standalone/tools/shm_ring_stats.c` | Print new fields |
| `star6e-standalone/tools/shm_consumer_test.c` | Print epoch/version |

## Build Verification

All 329 tests pass under `make test`, `make test-asan`, and `make test-tsan`.
