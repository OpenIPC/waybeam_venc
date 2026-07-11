# Maruko (Infinity6C) `video.framing` stab / stab-fill parity

Status: **Phase 0 + Phase 1 complete — GATE PASSED (see plan.md Phase 1 RESULTS).
Stab viable on Maruko; proceeding to Phase 2 (extract Kalman).**
Date: 2026-07-08
Devices: `root@192.168.2.233` (i6c / SSC378QE, OpenIPC, musl)

## Goal

Bring `video0.framing = stab` (and, if viable, `stab-fill`) to the Maruko backend
for parity with Star6E.

## Phase 0 — DONE: the IVE detector works on Maruko

`MI_IVE_Shift_Detector` is the motion detector both presets are built on. It
previously failed on Maruko at `MI_IVE_Create()`:

```
[ALGO_MSG_ERR] mmap failed!!
[ALGO_MSG_ERR] Unexpected IC version
[ALGO_MSG_ERR] The platform check is incorrect while MI_MVE_Init() is called!!
MI_IVE_Create(0) -> -1610604513
```

**Root cause: a userspace blob-vintage mismatch, NOT a kernel bug.**

- OpenIPC's i6c `msys` + `ive` kernel driver source is **byte-identical** to the
  vendor BSP's (`Maruko-ILS00_TINY_V1.4.0`, at `/home/snokvist/dev/Maruko/SourceCode`).
  Diff = 0 lines across `ms_msys.c`, `drv_ive.c`, `hal_ive.c`, `hal_ive_reg.h`,
  `mdrv_ive.c`, `hal_clk.c`.
- `0x5315` = `IOCTL_MSYS_GET_RIU_MAP` = `_IO('S',0x15)`; handler
  `msys_get_riu_map_verchk()` (`ms_msys.c:860`) `copy_to_user`s a packed 20-byte
  `MSYS_MMIO_INFO`: base(u64)@off4, size(u32)@off12; base = `Chip_Get_RIU_Phys()`
  = `IO_PHYS` (`arch/arm/mach-sstar/infinity6c/soc.c:211`).
- OpenIPC's blob (`MVX1##I6C#####d10fcfb#ive`) reads base@off4 / size@off12 —
  **exactly what the driver writes**. The "wrong struct offset" theory is refuted.
- The real difference: OpenIPC's **older** blob performs its IC-version platform
  check by raw-mmapping `/dev/mem` at the RIU base, and that mmap EINVALs on this
  firmware. The BSP-matched blob (`MVX1##I6C#####c6a1e30#ive`) performs the same
  check via `/dev/mstar_ive0` ioctls and **never opens `/dev/mem`** (`nm -D`: no
  raw `mmap`, no `/dev/mem` string).

**Fix: swap ONLY `libmi_ive.so`.** Use the BSP uClibc-9.1.0 variant
(md5 `d608368e2348855adab63667fc1d359c`, 446,908 B, vs OpenIPC's 754,672 B).

> **Do NOT also swap `libmi_sys.so` / `libmi_common.so`.** The BSP's uClibc
> `libmi_sys` **segfaults inside `MI_SYS_Init`** on our musl rootfs. `libmi_ive`
> needs only five stable MI_SYS symbols (`MI_SYS_MMA_Alloc/Free`,
> `MI_SYS_Mmap/Munmap`, `MI_SYS_FlushInvCache`), so it rides fine on the MI stack
> osdrv already installs. The rootfs is musl with uClibc-compat symlinks
> (`/lib/ld-uClibc.so.0`, `/lib/libc.so.0`), and the blob only `NEEDS`
> `libc.so.0` + `libgcc_s.so.1`, both present.

Packaged in `builder` as a file-level override:
`builder/package/sigmastar-osdrv-infinity6c/files/lib/libmi_ive.so`
(+ `README.waybeam.md`). `copy_extra_packages()` merges it into the upstream osdrv
package, so it survives `builder.sh`'s `rm -rf openipc`; osdrv's `_LIBRARIES` hook
installs it. Single writer for `/usr/lib/libmi_ive.so`, no install-order race under
`BR2_PER_PACKAGE_DIRECTORIES`.

### Verified on device (`.233`, clean boot)

```
MI_SYS_Init(0)    -> 0
MI_IVE_Create(0)  -> 0
MI_IVE_Destroy(0) -> 0
MI_SYS_Exit(0)    -> 0

MI_IVE_Shift_Detector, 384x384 crop / box 256 / pyramid 3 / search 96:
  expected      raw(dx,dy)   status
  (  0,  0)  ->  (  0,  0)     OK
  (  2,  1)  ->  (  2,  1)     OK
  (  5,  3)  ->  (  5,  3)     OK
  ( -4,  2)  ->  ( -4,  2)     OK
  (  8, -6)  ->  (  8, -6)     OK
  5/5 recovered, exact magnitude AND sign
```

Tools (committed): `tools/ive_i6c_probe.c`, `tools/ive_i6c_shift.c`.

### Probe gotchas (cost hours; both were harness bugs, not blob bugs)

1. `libmi_sys.so` has undefined `CamOsTsem{Init,Up,Down,Deinit}`, which live in
   **`libcam_os_wrapper.so`**. Under `RTLD_LAZY` they resolve on *first call* —
   inside `MI_SYS_Init` — and **SIGSEGV** unless the wrapper is preloaded
   `RTLD_GLOBAL` first.
2. `MI_SYS_Init` takes a **`u16 soc_id`** (`include/maruko_mi.h:65`
   `int (*fnInit)(uint16_t)`; `include/star6e.h:156`
   `#define MI_SYS_Init() g_mi_sys.fnInit(0)`). Calling it through a
   void-signature fn-ptr leaves `r0` undefined → junk `soc_id` →
   `ioctl 0x80046900` EINVAL.

## The cost model — set expectations honestly

`MI_IVE_Shift_Detector` is **NOT hardware-accelerated**. Measured on `.233`:
**16.810 ms/call wall, 16.797 ms/call CPU = 100% of one A7 core**, with the
`ive isr` (IRQ 78) counter pinned at **0** across 100+ calls.

Why: `libmi_ive.so` contains two modules — `#ive#` (the HW shim) and `#MVE#`,
which is **SigmaStar's Simd/NEON software vision library**, not a "Motion Vector
Engine". `MI_IVE_Shift_Detector` → `MI_MVE_Shift_Detector` →
`MI_MVE_Shift_Detector_Simd` → `Simd::Neon::SquaredDifferenceSum`. No `ioctl` on
that path. There is no block-match op in the IVE hardware ISA and no
motion/mv_/block_match symbol anywhere in `drivers/sstar/`.

Consequences:
- Budget stab on Maruko at roughly Star6E's per-frame cost (~17 ms vs ~19 ms).
- **Stab at 60 fps is arithmetically impossible with the full detector config**
  (16.8 ms > the 16.7 ms frame budget), and Maruko already burns ~60% of its
  single A7 core at 90–100 fps. Expect an fps downgrade (Star6E: imx335 60→40).
- Star6E's cheapened config (256 crop / 128 box / 2-level pyramid) is the lever,
  at a documented quality cost (`src/star6e_framing_stab.c:193-218` warns it looks
  visibly jittery).
- **A self-contained CPU/NEON block-match detector is NOT needed.** That plan is
  cancelled — the vendor detector works and is already NEON.

## Scope

**In scope:** `framing=stab` on Maruko (Phases 1–4, ALL DONE — see `plan.md`).
**`framing=stab-fill`:** now PLANNED (user-requested 2026-07-09), Phase 5 in
`plan.md`. Gated on a pivotal unknown (Phase 5a): can the i6c VENC accept manually
pushed input frames (`MI_SYS_ChnInputPortGetBuf/PutBuf`)? Star6E's compose path
depends on it; Maruko VENC is RING_DMA-fed today. Bench 5a before the full port; if
it fails, fall back to a DIVP/RGN HW compose (its own spec).

## Acceptance criteria

1. `video0.framing=stab` validates and applies on Maruko (no 501 `not_implemented`).
2. Detector runs at sensor fps (or a consciously-chosen decimation) without
   MI_SYS MMU-callback storms or watchdog reset.
3. Stabilization is visibly correct: pan/zoom still work, `pause_stab` glides home.
4. Teardown is clean across ≥10 start/stop cycles (no zombie, no reboot needed).
5. WebUI no longer greys out the stab knobs on Maruko.

## Non-goals

- Hardware-accelerating the detector (impossible; see cost model).
- Gyro/IMU fusion (the seam exists at `star6e_framing_stab.c:1316` but is unused).
