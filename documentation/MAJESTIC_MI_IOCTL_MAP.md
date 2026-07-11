# Majestic vs waybeam — MI-SDK ioctl map & teardown lessons (Maruko / I6C)

Reverse-engineered 2026-07-05 while chasing the IMX335 2592×1944@59 gap (majestic 59 fps
clean vs waybeam 29.6 fps + ISP P0 FIFO storm on the *same* PR#83 driver). Captured with an
`LD_PRELOAD` `ioctl()` shim on `/dev/mi_*` (thread-safe, single atomic `write()` per record,
follows the `arg[2]` user pointer to dump the real attribute struct). Function names were
recovered by disassembling `/usr/bin/majestic` (statically-linked MI SDK, names stripped to
local): the MI wrappers are laid out consecutively in ioctl-nr order, each doing one ioctl,
and each caller's `.rodata` error string names the function.

**All MI modules share ioctl type `'i'` (0x69); the device node disambiguates.** Cmd word
= `_IOW('i', nr, size)` → `0x40000000 | (size<<16) | (0x69<<8) | nr`.

## Confidently decoded functions (hard `.rodata` string evidence)

### /dev/mi_sys
| cmd | nr | size | function | notes |
|---|---|---|---|---|
| `0x40386903` | 3 | 56 | **MI_SYS_BindChnPort2** | both apps, 3–4× (VIF→ISP, ISP→SCL, SCL→VENC, +snapshot). Payload = srcChnPort{modId,dev,chn,port} + dstChnPort + frmrate + **linkType** (last u32: 4=REALTIME, 16=RING, 1=FRAMEBASE). |
| `0x40386904` | 4 | 56 | **MI_SYS_UnBindChnPort** | **majestic only** — defensive unbind before (re)bind + in teardown. Payload = src+dst ChnPort. |
| `0x40186915` | 21 | 24 | **MI_SYS_SetChnOutputPortDepth** | **waybeam only** — `(port, usrDepth, bufQuota)`. majestic never sets port depths. |
| `0x4008691e` | 30 | 8 | **MI_SYS_FlushInvCache** | majestic per-frame (~per-frame userspace frame read); `(vaddr,len)`. 32-B variant `0x4020691e` also exists. |

### /dev/mi_isp
| cmd | nr | size | function | notes |
|---|---|---|---|---|
| `0x40046901` | 1 | 4 | **MI_ISP_CreateDevice** | `{DevId}` |
| `0x40586902` | 2 | 88 | **MI_ISP_CreateChannel** | 88-B `MI_ISP_ChannelAttr_t` |
| `0x40086903` | 3 | 8 | **MI_ISP_DestroyChannel** | `{dev,chn}` — teardown |
| `0x401c6906` | 6 | 28 | **MI_ISP_SetChnParam** | `MI_ISP_ChnParam_t` = `{eHDRType, e3DNRLevel, bMirror, bFlip, eRot, bY2bEnable}` |
| `0x40086909` | 9 | 8 | **MI_ISP_StartChannel** | `{dev,chn}` |
| `0x4008690a` | 10 | 8 | **MI_ISP_StopChannel** | `{dev,chn}` — teardown |
| `0x4020690b` | 11 | 32 | **MI_ISP_SetOutputPortParam** | output-port res/crop/fmt/compress |
| `0x400c690d` | 13 | 12 | **MI_ISP_EnableOutputPort** | `{dev,chn,port}` |
| `0x400c690e` | 14 | 12 | **MI_ISP_DisableOutputPort** | `{dev,chn,port}` — teardown/runtime-reconfigure |

Per-frame ISP reads seen on both (scale with fps, not causal): `0x401c6911` (nr17, 28 B) and
`0x401c6912` (nr18, 28 B) — ISP stats/AE. `0xc02c6923` (nr35, 44 B, `_IOWR`) also per-frame-ish.

> Caveat: exact cmd↔function pairing for a couple of the *lifecycle* entries was first derived
> from a partially-corrupted (pre-thread-safe) log; the **function names are solid** (`.rodata`),
> and the **clean steady-state inventory below is authoritative** for which ioctls actually occur.

## Clean steady-state ioctl inventory (12 s @ 2592×1944@59, counts maj / wb)

```
mi_sys : BindChnPort2 4/3 · UnBindChnPort 0/0(steady) · SetChnOutputPortDepth 0/4 · nr32(68B) 2/2 · nr0(4B) 0/1
mi_isp : CreateChannel(88B) 1/1 · SetChnParam(28B) 1/1 · SetOutputPortParam(32B) 1/1 · EnableOutputPort(12B) 1/1
         StartChannel-ish(8B) 1/1 · nr0(8B) 1/1 · [per-frame] nr17 1955/858 · nr18 7181/3456 · nr35 2/1
mi_vif : nr0(32B) 1/1 · nr3(24B,SetDevAttr) 1/1 · nr5(4B) 1/1 · nr8(32B) 1/1 · nr10(8B) 1/1
mi_scl : nr0/8/2/6/12/10 all 1–2 / 1–2 (identical)
mi_sensor: majestic ~2 calls total; **waybeam continuously polls** (nr8 ×8, nr9 ×7, nr1/5/11/12/14 wb-only)
```

**Key steady-state finding:** every *configuration* ioctl (VIF/ISP/SCL attrs, binds, link
types) is **byte-identical** between the two apps. The per-frame counts differ only ~2×,
tracking the 59-vs-29.6 fps ratio. So the fps gap is **not** a configuration difference.

## Lessons for waybeam — teardown / wedge / zombie hardening

> **CORRECTION (2026-07-05):** this section was drafted from the experiment branch before the
> current master teardown was audited. A full code audit —
> **`documentation/MARUKO_PIPELINE_HARDENING.md`** — found the premise below is **stale**: master
> already implements or deliberately supersedes every item here. See that doc's §2 verdict table.
> The original majestic observations are kept below for reference, each annotated with the
> audited reality.

majestic performs an ordered teardown + defensive unbind. When first written this looked like
something waybeam lacked; the audit showed waybeam already does it (and more). Annotated:

1. **Ordered ISP teardown** — majestic does `DisableOutputPort → StopChannel → DestroyChannel`
   then `MI_SYS_UnBindChnPort` per pair. **Reality: already done, more thoroughly.**
   `maruko_pipeline_teardown_graph` (`maruko_pipeline.c:4016-4176`) is a documented 8-step ordered
   teardown that UnBinds all three binds (flag-guarded) *and* drains in-flight buffers first
   (`maruko_wait_output_idle`) — a step majestic omits.
2. **Defensive UnBind-before-Bind** in the setup path. **Reality: not needed.** Maruko fork+exec
   **respawns** on every reinit (`maruko_runtime.c:118-156`), so `bind_maruko_pipeline` always runs
   in a virgin process with nothing pre-bound.
3. **Runtime reconfigure without full teardown.** **Reality: deliberately rejected.** In-process
   reconfigure page-faulted and zombied the SoC (`maruko_runtime.c:132-151`); the respawn model
   exists specifically to avoid it. Adopting this would regress.
4. **Symbols already resolved** (`maruko_mi.c`) — true, and the teardown that uses them already
   exists.

**Net: no adoption checklist survives the audit.** The only residual work identified is cleanup
(remove dead `maruko_stop_vpe`, refresh stale comments) — done in
`MARUKO_PIPELINE_HARDENING.md` §4. The teardown-only ISP symbols (`MI_ISP_DestroyChannel` nr3,
`MI_ISP_StopChannel` nr10) are intentionally *not* bound: master skips `ISP DestroyChannel` to
avoid a CUS3A-mutex crash and lets `MI_SYS_Exit` reclaim the channel.

## fps-gap status (config exhausted)

**Every configuration ioctl is byte-identical**, and every runtime lever tested came back
negative — buffer depths (set *and* removed), 3A/AE freeze, 3DNR off, ISP output format, IFC
compression, snapshot path, VIF attrs, ISP `ChnParam`/`OutputPortParam`, and a post-start ISP
output-port disable→re-enable **kick** (fired but did **not** release the half-rate). So the
29.6-vs-59 REALTIME gap is **not reachable through any MI-SDK config surface** we can drive.

The only remaining un-equalized difference is workload, not config: the ioctl capture was
**bare majestic**, whereas waybeam runs the full app alongside the encode (HTTP, mDNS, metrics,
OSD, IMU, RTP, and — visible in the inventory above — a **continuous `mi_sensor` poll** majestic
never issues). The SDK FAQ pins ISP P0 FIFO-FULL on *bandwidth starvation*, so **DRAM/CPU
bandwidth contention from waybeam's ancillary threads** is the leading unresolved hypothesis.
The proven clean full-res fallback is **FRAME_BASE @49 fps** — see
`documentation/MARUKO_IMX335_FRAMEBASE_49FPS.md`.

Full elimination trail + method in memory `[[maruko_imx335_majestic_isp_wall_crossvalidation]]`.
The reusable pointer-following, thread-safe `ioctl()` shim used for this capture is preserved in
that investigation's scratchpad (single atomic `write()` per record; follows the `arg[2]` user
pointer to dump the real MI attribute struct) — re-buildable from the source there if this map
needs to be re-verified on another driver/mode.
