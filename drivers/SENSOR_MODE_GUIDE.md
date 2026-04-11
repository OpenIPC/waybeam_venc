# IMX415 Sensor Mode Calculation Guide (Maruko / SSC378QE)

How to create custom resolution modes for the IMX415 sensor on the
SigmaStar Infinity6C (Maruko) platform. Based on empirical testing
and register analysis of working modes.

## Approach

All custom modes are derived from the **proven 120fps binning init
table** by changing only the PIX crop registers. The analog config,
MIPI timing, clock PLLs, and binning registers stay identical. This
minimizes risk — the only variable is the sensor readout window size.

## Architecture

```
Full IMX415 sensor (3864 x 2192 Bayer cells)
  └─ PIX crop window (PIX_HST, PIX_HWIDTH, PIX_VST, PIX_VWIDTH)
      └─ 2×2 binning (HADD=1, VADD=1, ADDMODE=1)
          └─ MIPI output to ISP (target_width × target_height)
```

## Register Calculations

### Step 1: Target resolution → PIX crop window

Given a target output resolution (W × H) with 2×2 binning:

```
PIX_HWIDTH = W × 2       (horizontal: 2 Bayer columns per output pixel)
PIX_VWIDTH = H × 4       (vertical: 4 Bayer rows per output pixel with binning)
```

Examples:
| Target | PIX_HWIDTH | PIX_VWIDTH |
|--------|-----------|-----------|
| 1472×816 (proven) | 2952 (0x0B88) | 3264 (0x0CC0) |
| 1600×900 (proven) | 3200 (0x0C80) | 3600 (0x0E10) |
| 1760×990 | 3520 (0x0DC0) | 3960 (0x0F78) |
| 1920×1080 (fails) | 3840 (0x0F00) | 4320 (0x10E0) |

### Step 2: Center the crop window

```
PIX_HST = (3864 - PIX_HWIDTH) / 2    (round to even)
PIX_VST = (3836 - PIX_VWIDTH) / 2    (round to even, use 0 if negative)
```

The vertical total (3836) is derived from the 120fps mode:
PIX_VST(548) + PIX_VWIDTH(3288) = 3836.

Examples:
| Target | PIX_HST | PIX_VST |
|--------|---------|---------|
| 1472×816 | 456 (0x01C8) | 274 (0x0112) |
| 1600×900 | 332 (0x014C) | 118 (0x0076) |
| 1760×990 | 172 (0x00AC) | 0 (near limit) |
| 1920×1080 | 12 (0x000C) | 0 (exceeds) |

Note: The actual 120fps mode uses PIX_HST=468, PIX_VST=548 which
doesn't match centered exactly — alignment constraints may apply.
Use the calculated values as starting points and adjust if needed.

### Step 3: Encode as register bytes

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

### Step 4: VTS for target FPS

VTS (VMAX register) controls frame rate. All modes share the same
line period (4882 ns, from HMAX=0x016D at INCKSEL3=0xC6):

```
VTS = 1700 × 120 / target_fps
```

| FPS | VTS | VMAX bytes |
|-----|-----|-----------|
| 120 | 1700 | 0x06, 0xA4 |
| 90 | 2267 | 0x08, 0xDB |
| 60 | 3400 | 0x0D, 0x48 |
| 30 | 6800 | 0x1A, 0x90 |

VTS only needs to be set in the `pCus_SetVideoRes` function via
`vts_30fps` — the sensor driver's `SetFPS` callback adjusts the
actual VMAX register dynamically. The init table's VMAX is just
the startup default and gets overridden.

### Step 5: SetVideoRes entry

```c
case N: // WxH@Ffps
    handle->video_res_supported.ulcur_res = N;
    handle->pCus_sensor_init = pCus_init_WxH_Ffps_mipi4lane_linear;
    vts_30fps = VTS_VALUE;
    params->expo.vts = vts_30fps;
    params->expo.fps = F;
    Preview_line_period = 4882;  // constant for all binned modes
    break;
```

### Step 6: Mode table entry

```c
{ LINEAR_RES_N, { W, H, 3, F }, { 0, 0, W, H }, { "WxH@Ffps" } },
```

## What stays constant (DO NOT change)

These registers are identical across all binned modes and must not
be modified — they define the analog signal chain, PLL clocks, and
MIPI physical layer:

| Register | Value | Purpose |
|----------|-------|---------|
| 0x3008 | 0x5D | BCWAIT_TIME |
| 0x300A | 0x42 | CPWAIT_TIME |
| 0x3020 | 0x01 | HADD (binning) |
| 0x3021 | 0x01 | VADD (binning) |
| 0x3022 | 0x01 | ADDMODE (2/2) |
| 0x3028-29 | 0x016D | HMAX (line time) |
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

## What changes per mode

| Register | Depends on | Formula |
|----------|-----------|---------|
| 0x3040-41 | PIX_HST | (3864 - W×2) / 2 |
| 0x3042-43 | PIX_HWIDTH | W × 2 |
| 0x3044-45 | PIX_VST | (3836 - H×4) / 2 |
| 0x3046-47 | PIX_VWIDTH | H × 4 |
| VMAX (via VTS) | FPS | 1700 × 120 / fps |

## Known ISP limits (SSC378QE firmware)

| Resolution | Status | Notes |
|-----------|--------|-------|
| 1472×816 | WORKS | All FPS (30-120) |
| 1600×900 | WORKS | Tested at 30fps |
| 1920×1080 | FAILS | ISP Skip IQ, Fcnt=0 |

The ISP limit is between 1600×900 and 1920×1080. Binary search
recommended: try 1760×990, then 1680×945 or 1856×1044.

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

## Applying to other sensors (IMX335, etc.)

The same approach works for any sensor on this platform:

1. Find a working mode (any resolution/FPS that produces frames)
2. Identify the init table registers that define the crop window
3. Keep all other registers (analog, PLL, MIPI) identical
4. Adjust only the crop window and VTS for new resolution/FPS
5. Binary search to find the ISP's maximum input resolution

The ISP resolution limit is a property of the SSC378QE firmware,
not the sensor. The same limit applies regardless of sensor model.
