# Known Issues

## Maruko: encoder stall after output disable/re-enable

**Status:** Open
**Backend:** Maruko only (Star6E unaffected)
**Discovered:** 2026-04-11 during API test suite run

### Symptom

When `outgoing.enabled` is set to `false` (output disable) and then back
to `true` (re-enable), the Maruko encoder stops producing frames. The
stream loop logs `waiting for encoder data...` repeatedly and eventually
aborts with `ERROR: no encoder data received; aborting stream loop`.

After this failure, the MI VENC kernel module is left in a bad state —
subsequent venc restarts (without device reboot) also fail to produce
frames.

### Impact

- API test suite section 11 ("Output Enable/Disable Toggle") fails on Maruko
- The test suite's cleanup/restore logic cannot run after the failure,
  leaving `outgoing.enabled=false` and `verbose=false` in the live config
- Requires device reboot to recover

### Root cause (preliminary)

The Maruko output disable path reduces FPS to 5 (idle mode) and unbinds
the VENC channel. On re-enable, the rebind or FPS restore does not
properly restart the encoder pipeline. The MI_VENC_StartRecvPic call
may succeed but the SCL→VENC bind is not re-established, so no frames
flow to the encoder.

Star6E does not have this issue because its VPE→VENC bind path handles
the rebind correctly.

### Workaround

Reboot the device after encountering this failure:
```
reboot
```

### Reproduction

```bash
# With venc running on Maruko (192.168.2.12):
wget -q -O- 'http://192.168.2.12:80/api/v1/set?outgoing.enabled=false'
sleep 2
wget -q -O- 'http://192.168.2.12:80/api/v1/set?outgoing.enabled=true'
# Stream will not resume — check /tmp/venc.log for "waiting for encoder data"
```
