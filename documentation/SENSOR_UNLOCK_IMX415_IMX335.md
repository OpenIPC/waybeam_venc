# IMX415 Cold-Boot High-FPS Unlock (Star6E Standalone)

This document captures the proven initialization sequence required to unlock
IMX415 high-FPS sensor modes on cold boot in the standalone Star6E path.

Scope:
- Target: the repo root only
- Device class: SigmaStar Star6E / SSC338Q
- Sensor: IMX415 (observed on this board family)

Compatibility note:
- `sensor_imx335_mipi.ko` was inspected and contains the same unlock gate and
  custom command behavior (`cmd=0x23`, `reg=0x300a`, latch `0x430` -> `0x80`).
- So this unlock sequence is compatible with IMX335 driver logic as well.
- Runtime validation in this session was performed on IMX415 hardware; IMX335
  camera runtime should still be validated on a board with IMX335 attached.

## Problem

On cold boot, `MI_SNR_SetRes` could return success while the active mode stayed
stuck at mode index `0` (`3840x2160@30`). Then `MI_SNR_SetFps(60/90/120)` failed
with `-1608835041` and the pipeline remained at 30fps.

Running `majestic` once could "unlock" high-FPS, proving hardware capability.

## Root Cause (Driver-Level)

From `sensor_imx415_mipi.ko` analysis:
- `pCus_SetVideoRes` checks an internal latch (driver state offset `0x430`).
- If latch is not `0x80`, mode selection falls back to mode `0` (`4K30` path).
- `pCus_sensor_CustDefineFunction` command `0x23` can set this latch using a
  payload interpreted as two `u16` values: `{reg, data}`.
- For `reg == 0x300a`, `data == 0x80` enables the high-FPS path.

## Required Initialization Sequence

Use this order before sensor enable:

1. `MI_SNR_SetPlaneMode(pad, E_MI_SNR_PLANE_MODE_LINEAR)`
2. `MI_SNR_CustFunction(pad, 0x23, 4, {0x300a, 0x80}, E_MI_SNR_CUSTDATA_TO_DRIVER)`
3. `MI_SNR_SetRes(pad, mode_index)`
4. `MI_SNR_SetFps(pad, requested_fps)`
5. `MI_SNR_Enable(pad)`

If you do a retry sequence that disables/reconfigures sensor mode, re-apply step 2
again before `SetRes`.

## Minimal C Example

```c
typedef struct {
  MI_U16 reg;
  MI_U16 data;
} SensorUnlockPayload;

MI_S32 imx415_unlock(MI_SNR_PAD_ID_e pad) {
  SensorUnlockPayload p = {.reg = 0x300a, .data = 0x80};
  return MI_SNR_CustFunction(pad, 0x23, sizeof(p), &p, E_MI_SNR_CUSTDATA_TO_DRIVER);
}

MI_S32 init_sensor_unlock_then_mode(MI_SNR_PAD_ID_e pad, MI_U32 mode, MI_U32 fps) {
  MI_S32 ret;
  ret = MI_SNR_SetPlaneMode(pad, E_MI_SNR_PLANE_MODE_LINEAR); if (ret) return ret;
  ret = imx415_unlock(pad); if (ret) return ret;
  ret = MI_SNR_SetRes(pad, mode); if (ret) return ret;
  ret = MI_SNR_SetFps(pad, fps); if (ret) return ret;
  ret = MI_SNR_Enable(pad); if (ret) return ret;
  return 0;
}
```

## Standalone `venc` Integration

The current runtime path exposes the unlock settings through `/etc/venc.json`
and the live config model:

- `sensor.unlockEnabled`
- `sensor.unlockCmd` (default `0x23`)
- `sensor.unlockReg` (default `0x300a`)
- `sensor.unlockValue` (default `0x80`)
- `sensor.unlockDir` (default `0`, driver direction)

Recommended default:
- Keep defaults for IMX415 cold-boot operation.

Low-level probe binaries still use their CLI flags for unlock experiments.

## Reproducible Test Procedure (Cold State)

Important:
- Reboot before each cold claim.
- If `majestic` has been run, reboot first.
- `/tmp` is volatile; re-upload binaries after reboot.

### 1) Probe-level verification

Success path (`120fps`):

```bash
scripts/remote_test.sh \
  --reboot-before-run --timeout-sec 45 --run-bin snr_sequence_probe -- \
  --sensor-index 0 --stage1-mode 3 --stage1-fps 120 --skip-stage2 \
  --cust-pre --cust-cmd 0x23 --cust-size 4 --cust-value 0x0080300a --cust-dir 0
```

Expected:
- `MI_SNR_SetFps(120) -> 0`
- `after-stage1 ... idx=3 1472x816 ... fps=120`

Negative control (`0x40`, should fail):

```bash
scripts/remote_test.sh \
  --reboot-before-run --timeout-sec 45 --run-bin snr_sequence_probe -- \
  --sensor-index 0 --stage1-mode 1 --stage1-fps 30 --stage2-mode 3 --stage2-fps 120 \
  --cust-pre --cust-cmd 0x23 --cust-size 4 --cust-value 0x0040300a --cust-dir 0
```

Expected:
- `after-stage2 ... idx=0 3840x2160 ... fps=30`
- `MI_SNR_SetFps(120) -> -1608835041`

### 2) End-to-end `venc` verification

Current runtime path: update `/etc/venc.json`, then restart through the
direct helper.

`120fps` mode:

```bash
scripts/star6e_direct_deploy.sh config-set .sensor.index 0
scripts/star6e_direct_deploy.sh config-set .sensor.mode 3
scripts/star6e_direct_deploy.sh config-set .video0.fps 120
scripts/star6e_direct_deploy.sh config-set .video0.size '"1472x816"'
scripts/star6e_direct_deploy.sh config-set .isp.sensorBin '"/etc/sensors/imx415_greg_fpvXVIII-gpt200.bin"'
scripts/star6e_direct_deploy.sh cycle
```

`90fps` mode:

```bash
scripts/star6e_direct_deploy.sh config-set .sensor.index 0
scripts/star6e_direct_deploy.sh config-set .sensor.mode 2
scripts/star6e_direct_deploy.sh config-set .video0.fps 90
scripts/star6e_direct_deploy.sh config-set .video0.size '"1280x720"'
scripts/star6e_direct_deploy.sh config-set .isp.sensorBin '"/etc/sensors/imx415_greg_fpvXVIII-gpt200.bin"'
scripts/star6e_direct_deploy.sh cycle
```

Expected:
- Sensor mode readback matches requested mode.
- FPS no longer forced to 30 on cold boot.
- HTTP API becomes reachable and the stream stays stable.

## Troubleshooting

- If high FPS still fails, confirm unlock settings are default:
  - cmd `0x23`, reg `0x300a`, value `0x80`, dir `0`.
- Confirm cold state (rebooted after any `majestic` run).
- Check dmesg delta for sensor errors:
  - `MI_SNR_IMPL_SetFps ... fail`
- Keep timeouts on remote runs to avoid lockups; power-cycle if SSH becomes unresponsive.
