# IMX415 Sensor Mode Calculation Guide (Maruko / SSC378QE)

How to create custom resolution modes for the IMX415 sensor on the
SigmaStar Infinity6C (Maruko) platform. Based on empirical testing
and register analysis of working modes.

## Final Mode Table

| Mode | Resolution | FPS | Type | FOV | Notes |
|------|-----------|-----|------|-----|-------|
| 0 | 3760x2116 | 30 | Non-binned | 97% | Best quality, near-full FOV |
| 1 | 3760x1024 | 59 | Non-binned superwide | 97% H | Max non-binned width at ~60fps |
| 2 | 1920x1080 | 60 | 2x2 binned | 99% | Standard HD, widest FOV |
| 3 | 1920x1080 | 90 | 2x2 binned | 99% | High frame rate HD |
| 4 | 1472x816 | 120 | 2x2 binned | 76% | Maximum frame rate |

## Architecture

```
Full IMX415 sensor (3864 x 2192 Bayer cells)
  |- BINNED path (modes 2-4):
  |    PIX crop window (PIX_HWIDTH = W*2, PIX_VWIDTH = H*4)
  |      -> 2x2 binning (HADD=1, VADD=1, ADDMODE=1)
  |        -> MIPI output to ISP (W x H)
  |
  |- NON-BINNED path (modes 0-1):
       PIX crop window (PIX_HWIDTH = W, PIX_VWIDTH = H*2)
         -> No binning (HADD=0, VADD=0, ADDMODE=0)
           -> MIPI output to ISP (W x H)
```

## FOV Analysis: Binned vs Non-Binned

With 2x2 binning, PIX_HWIDTH = output_width x 2. So binned modes read
MORE sensor columns for the same ISP input width:

| Mode | PIX_HWIDTH | H FOV % | Notes |
|------|-----------|---------|-------|
| Binned 1920x1080 | 3840 | 99.4% | Widest possible FOV |
| Non-binned 3760x2116 | 3760 | 97.3% | Near-max non-binned width |
| Non-binned 2560x1440 | 2560 | 66.3% | Significant center crop |
| Binned 1472x816 | 2944 | 76.2% | Narrower due to ISP throughput limit |

Key insight: binned modes give wider FOV than non-binned at the same
ISP input resolution because they sample more sensor area.

## Register Calculations

### Binned modes (2x2 binning)

Given a target output resolution (W x H):

```
PIX_HWIDTH = W x 2       (horizontal: 2 Bayer columns per output pixel)
PIX_VWIDTH = H x 4       (vertical: 4 Bayer rows per output pixel with binning)
PIX_HST = ((3864 - PIX_HWIDTH) / 2) & ~7    (8-aligned center)
PIX_VST = ((3836 - PIX_VWIDTH) / 2) & ~7    (8-aligned center, 0 if negative)
```

The vertical total (3836) is derived from the 120fps mode:
PIX_VST(548) + PIX_VWIDTH(3288) = 3836.

Examples:
| Target | PIX_HWIDTH | PIX_VWIDTH | PIX_HST | PIX_VST |
|--------|-----------|-----------|---------|---------|
| 1472x816 | 2952 (0x0B88) | 3264 (0x0CC0) | 456 (0x01C8) | 288 (0x0120) |
| 1920x1080 | 3840 (0x0F00) | 4320 (0x10E0) | 12 (0x000C) | 0 |

### Non-binned modes (dynamic init)

Given a target output resolution (W x H):

```
PIX_HWIDTH = W           (direct, no multiplier)
PIX_VWIDTH = H x 2       (Bayer row pairs)
PIX_HST = ((3864 - PIX_HWIDTH) / 2) & ~7    (8-aligned center)
PIX_VST = ((4384 - PIX_VWIDTH) / 2) & ~7    (8-aligned center, 0 if negative)
```

The driver uses a **dynamic init function** that computes PIX crop registers
from the mode table resolution at runtime. This eliminates the need for
separate init tables per non-binned resolution.

Examples:
| Target | PIX_HWIDTH | PIX_VWIDTH | PIX_HST | PIX_VST |
|--------|-----------|-----------|---------|---------|
| 3760x2116 | 3760 | 4232 | 48 (0x0030) | 72 (0x0048) |
| 3760x1024 | 3760 | 2048 | 48 (0x0030) | 1168 (0x0490) |
| 2560x1440 | 2560 | 2880 | 648 (0x0288) | 752 (0x02F0) |

### Encode as register bytes

PIX registers are 16-bit, stored little-endian in two I2C writes:

```c
// PIX_HST = 332 = 0x014C
{ 0x3040, 0x4C },  // PIX_HST low byte
{ 0x3041, 0x01 },  // PIX_HST high byte

// PIX_HWIDTH = 3200 = 0x0C80
{ 0x3042, 0x80 },  // PIX_HWIDTH low byte
{ 0x3043, 0x0C },  // PIX_HWIDTH high byte

// PIX_VST = 118 = 0x0076
{ 0x3044, 0x76 },  // PIX_VST low byte (round to even: 0x76)
{ 0x3045, 0x00 },  // PIX_VST high byte

// PIX_VWIDTH = 3600 = 0x0E10
{ 0x3046, 0x10 },  // PIX_VWIDTH low byte
{ 0x3047, 0x0E },  // PIX_VWIDTH high byte
```

### VTS for target FPS

**Binned modes (HMAX=365):** line period = 4882 ns

```
VTS = 1700 x 120 / target_fps
```

| FPS | VTS | VMAX bytes |
|-----|-----|-----------|
| 120 | 1700 | 0x06, 0xA4 |
| 90 | 2267 | 0x08, 0xDB |
| 60 | 3400 | 0x0D, 0x48 |
| 30 | 6800 | 0x1A, 0x90 |

**Non-binned modes (HMAX=1100):** line period = ~14815 ns

| FPS | VTS | Notes |
|-----|-----|-------|
| 30 | 2250 | Standard |
| 59 | ~1125 | Max height ~1024 at this VTS |

VTS only needs to be set in the `pCus_SetVideoRes` function via
`vts_30fps` -- the sensor driver's `SetFPS` callback adjusts the
actual VMAX register dynamically. The init table's VMAX is just
the startup default and gets overridden.

### SetVideoRes entry

```c
case N: // WxH@Ffps
    handle->video_res_supported.ulcur_res = N;
    handle->pCus_sensor_init = pCus_init_WxH_Ffps_mipi4lane_linear;
    vts_30fps = VTS_VALUE;
    params->expo.vts = vts_30fps;
    params->expo.fps = F;
    Preview_line_period = LINE_PERIOD;  // 4882 binned, ~14815 non-binned
    break;
```

### Mode table entry

```c
{ LINEAR_RES_N, { W, H, 3, F }, { 0, 0, W, H }, { "WxH@Ffps" } },
```

## What stays constant (DO NOT change)

These registers are identical across all binned modes and must not
be modified -- they define the analog signal chain, PLL clocks, and
MIPI physical layer:

| Register | Value | Purpose |
|----------|-------|---------|
| 0x3008 | 0x5D | BCWAIT_TIME |
| 0x300A | 0x42 | CPWAIT_TIME |
| 0x3028-29 | 0x016D | HMAX (binned, 365) |
| 0x3031 | 0x00 | ADBIT (10-bit ADC) |
| 0x3033 | 0x05 | SYS_MODE (891Mbps) |
| 0x3116 | 0x23 | INCKSEL2 |
| 0x3118 | 0xC6 | INCKSEL3 (I6C clock) |
| 0x311A | 0xE7 | INCKSEL4 |
| 0x311E | 0x23 | INCKSEL5 |
| 0x301C | 0x04 | WINMODE (crop mode) |
| 0x30D9 | 0x02 | DIG_CLP_VSTART |
| 0x30DA | 0x01 | DIG_CLP_VNUM |
| 0x4004-4028 | various | MIPI PHY timing |
| 0x3700-3BCA | various | Analog front-end |

**Binned-only registers** (must explicitly set to 0 for non-binned):

| Register | Binned | Non-binned | Purpose |
|----------|--------|-----------|---------|
| 0x3020 | 0x01 | 0x00 | HADD |
| 0x3021 | 0x01 | 0x00 | VADD |
| 0x3022 | 0x01 | 0x00 | ADDMODE (2/2) |

## What changes per mode

### Binned modes (2x2 binning, HMAX=365)

| Register | Depends on | Formula |
|----------|-----------|---------|
| 0x3040-41 | PIX_HST | (3864 - W*2) / 2, 8-aligned |
| 0x3042-43 | PIX_HWIDTH | W x 2 |
| 0x3044-45 | PIX_VST | (3836 - H*4) / 2, 8-aligned |
| 0x3046-47 | PIX_VWIDTH | H x 4 |
| VMAX (via VTS) | FPS | 1700 x 120 / fps |

### Non-binned modes (HMAX=1100)

| Register | Depends on | Formula |
|----------|-----------|---------|
| 0x3020-22 | Binning | 0x00 (explicit disable required) |
| 0x301C | WINMODE | 0x04 (crop) |
| 0x3028-29 | HMAX | 0x044C (1100) |
| 0x3040-41 | PIX_HST | (3864 - W) / 2, 8-aligned |
| 0x3042-43 | PIX_HWIDTH | W (direct, no multiplier) |
| 0x3044-45 | PIX_VST | (4384 - H*2) / 2, 8-aligned (0 if negative) |
| 0x3046-47 | PIX_VWIDTH | H x 2 (Bayer row pairs) |
| VMAX (via VTS) | FPS | 2250 for 30fps |

**Critical: PIX_HST and PIX_VST must be 8-aligned** to preserve
correct RGGB Bayer pattern at the crop boundary. Misalignment
causes purple/green color tint.

**Critical: HADD/VADD/ADDMODE must be explicitly set to 0** for
non-binned modes. The sensor retains register state across soft
reboots -- omitting these registers leaves binning active.

## Non-binned init: dynamic approach

The driver uses a single dynamic init function for non-binned modes
instead of separate init tables per resolution. The function reads
the target resolution from the mode table and computes PIX crop
registers at runtime:

```c
// Dynamic PIX register computation (non-binned)
PIX_HWIDTH = width;                                    // direct
PIX_VWIDTH = height * 2;                               // Bayer row pairs
PIX_HST = ((3864 - PIX_HWIDTH) / 2) & ~7;             // 8-aligned center
PIX_VST = ((4384 - PIX_VWIDTH) / 2) & ~7;             // 8-aligned, 0 if negative
```

This approach:
- Eliminates duplicate init tables that differ only in PIX values
- Makes adding new non-binned resolutions trivial (just add a mode table entry)
- Reduces risk of copy-paste errors in register tables

## Non-binned 60fps base init table

Two base init tables exist for non-binned modes:

| Table | HMAX | MIPI rate | Use |
|-------|------|-----------|-----|
| 30fps base | 1100 | 891Mbps (SYS_MODE=0x05, INCKSEL3=0xC6) | Modes 0 |
| 60fps base | 1100 | 891Mbps (SYS_MODE=0x05, INCKSEL3=0xC6) | Mode 1 |

Both tables use identical clock and MIPI settings. The difference is
only in VTS/fps configured via SetVideoRes. The 60fps base was
originally attempted with HMAX=652/1485Mbps (from I6E 5M mode) but
that does not work on I6C (see constraints below).

### 60fps height constraint

With HMAX=1100 (I6C minimum for non-binned), line period is ~14815ns.
At 60fps: VTS = 1/(60 * 14815ns) = ~1125 lines. Since VTS must be
greater than output_height, maximum output height at 60fps is ~1024.
This gives us Mode 1: 3760x1024@59fps.

## ISP limits (SSC378QE firmware)

### Binary search results: non-binned dimension limits

| Resolution | FPS | Mpix/s | Status | Notes |
|-----------|-----|--------|--------|-------|
| 1920x1080 | 30 | 62 | WORKS | Correct color |
| 2560x1440 | 30 | 111 | WORKS | Needs 8-aligned PIX offsets |
| 3200x1800 | 30 | 173 | WORKS | |
| 3520x1980 | 30 | 209 | WORKS | |
| 3680x2070 | 30 | 228 | WORKS | |
| 3760x2116 | 30 | 238 | WORKS | 97.3% H FOV, max working width |
| 3840x2160 | 15 | 124 | FAILS | Hard ISP dimension limit |

**Key finding:** 3840 width fails even at 15fps (124 Mpix/s), while
3760x2116@30fps works at 238 Mpix/s. This proves the failure at 3840
is a **hard ISP dimension limit**, not a throughput issue. The
non-binned width ceiling is 3760.

### ISP throughput limits

| Resolution | FPS | Mpix/s | Status | Notes |
|-----------|-----|--------|--------|-------|
| 1920x1080 | 60 | 124 | WORKS | Binned mode 2 |
| 1920x1080 | 90 | 186 | WORKS | Binned mode 3 |
| 3760x2116 | 30 | 238 | WORKS | Non-binned mode 0 |
| 1920x1080 | 120 | 249 | FAILS | ISP_IRQ_WQ_FRAME_START overflow |

Throughput ceiling: between 238 and 249 Mpix/s. The 120fps 1920x1080
mode exceeds ISP processing capacity, triggering frame start IRQ
overflow. Mode 4 (1472x816@120fps = 144 Mpix/s) works because it
stays within budget.

### Binned modes (HMAX=365)

| Resolution | Status | Notes |
|-----------|--------|-------|
| 1472x816 | WORKS | All FPS (30-120) |
| 1600x900 | WORKS | Tested at 30fps |
| 1920x1080 | WORKS | 30/60/90fps |

### I6C MIPI clock constraints

| Config | HMAX | MIPI rate | Status |
|--------|------|-----------|--------|
| SYS_MODE=0x05, INCKSEL3=0xC6 | 1100 | 891Mbps | WORKS (I6C proven) |
| SYS_MODE=0x08, INCKSEL3=0xA5 | 1100 | 1485Mbps | FAILS (stalls pipeline) |
| SYS_MODE=0x05, INCKSEL3=0xC6 | 550 | 891Mbps | FAILS (below sensor min for non-binned) |

**1485Mbps MIPI does not work on I6C.** Must stay with 891Mbps
(SYS_MODE=0x05, INCKSEL3=0xC6) for all modes. This limits non-binned
HMAX to 1100 minimum.

## Hardware reset for mode switching

The sensor retains I2C register values across soft reboots because
the sensor chip stays powered (separate power rail from SoC). This
means stale registers from a previous mode (e.g. binning, DIG_CLP)
persist and cause dark image or incorrect output.

**Fix:** Call `pCus_HardwareReset(handle)` at the start of each
init function. This toggles the hardware RESET pin via the kernel
VIF driver:

```c
static void pCus_HardwareReset(ms_cus_sensor* handle) {
    ISensorIfAPI* sensor_if = handle->sensor_if_api;
    sensor_if->Reset(0, handle->reset_POLARITY);   // assert reset
    SENSOR_MSLEEP(5);
    sensor_if->Reset(0, !handle->reset_POLARITY);  // deassert
    SENSOR_MSLEEP(20);
}
```

Cost: ~25ms per mode init. Enables reliable switching between any
modes (binned <-> non-binned) without power cycle.

## ISP bin and AE behavior

- `MI_ISP_API_CmdLoadBinFile` loads the ISP bin for AE/AWB tuning
- `MI_ISP_IQ_ApiCmdLoadBinFile` reloads IQ parameters but **resets
  AE state** -- must be SKIPPED to avoid dark image regression
- The IQ Set/Get API works without the IQ bin reload
- `exposure: 0` in venc config auto-caps to frame period + SetFps
  kick for maximum FPS (118fps at 120fps mode)
- `exposure: N` (ms) allows user to trade FPS for brightness

## Build and deploy

```bash
# Build sensor .ko
cd drivers/
make sensor

# Deploy to device
scp sensor_imx415_maruko.ko root@192.168.2.12:/lib/modules/5.10.61/sigmastar/sensor_imx415_mipi.ko

# Reboot required for kernel module reload
ssh root@192.168.2.12 reboot

# Update venc config for new mode
# "mode": N, "size": "WxH", "fps": F
```

## Applying to IMX335 (and other sensors)

The methodology and many constraints transfer directly to other
sensors on the same I6C platform:

### What transfers directly (SoC properties, not sensor)

1. **Dynamic non-binned init approach** -- compute PIX registers from
   the mode table resolution at runtime instead of separate init
   tables per resolution
2. **Binary search methodology for ISP limits** -- ISP dimension and
   throughput limits are SoC firmware properties, not sensor properties.
   The same limits apply regardless of sensor model:
   - Hard width ceiling: 3760 works, 3840 fails (non-binned)
   - Throughput ceiling: ~238-249 Mpix/s
3. **I6C clock constraints** -- must use INCKSEL3=0xC6, SYS_MODE=0x05
   (891Mbps MIPI). 1485Mbps does not work on I6C.
4. **8-aligned PIX offsets** -- required for correct Bayer pattern at
   crop boundaries (same for any Bayer sensor)
5. **VTS > output_height constraint** -- for non-binned modes, VTS
   limits the maximum output height at a given FPS
6. **Hardware reset** -- needed for reliable mode switching on any
   sensor that stays powered across SoC reboots

### What differs per sensor

1. **HMAX minimum** -- varies by sensor (IMX415 non-binned min is 1100).
   The IMX335 will have its own minimum HMAX for each readout mode.
2. **Sensor array dimensions** -- IMX415 is 3864x2192 (effective Bayer).
   PIX centering formulas must be adjusted for the IMX335 array size.
3. **Binning register addresses and values** -- sensor-specific I2C
   registers (HADD, VADD, ADDMODE equivalents)
4. **Analog front-end and PLL registers** -- entirely sensor-specific,
   copy from a known-working IMX335 mode
5. **Line period** -- depends on HMAX and input clock, recalculate for
   IMX335's timing parameters

### Recommended IMX335 bring-up sequence

1. Get one binned mode working (any resolution/FPS that produces frames)
2. Copy the working init table, modify only PIX crop + VTS
3. Binary search for ISP limits (expect same 3760 width ceiling)
4. Test non-binned mode with explicit HADD/VADD/ADDMODE=0
5. Implement dynamic non-binned init using PIX computation
6. Validate FOV at each resolution against IMX335 datasheet
