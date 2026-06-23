#ifndef DEVICE_ID_H
#define DEVICE_ID_H

/*
 * device_id — SoC die ID (hardware-unique serial) reader.
 *
 * Reads the SigmaStar die ID natively from RIU registers via /dev/mem — the
 * same value `ipcinfo -i` prints, and the same method the OpenIPC ipctool
 * SigmaStar HAL uses (no SDK linkage, no shell-out). Used as the stable fleet
 * identity: the full 12-hex string is exposed via the HTTP API, and a short
 * suffix names the mDNS beacon (`waybeam-<suffix>.local`).
 *
 * Not all SoCs expose a die ID (e.g. ssc37x / Maruko reads all-zero); callers
 * must handle the unavailable case (empty string / false).
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read the SoC die ID as a 12-char uppercase hex string (e.g.
 * "47D1CEF5CB1D") into out.  Returns true and NUL-terminates on success;
 * returns false and sets out="" when no usable die ID exists (degenerate
 * all-zero / all-ones read, unsupported SoC, or /dev/mem unreadable).
 * `len` must be at least 13.
 */
bool device_id_serial(char *out, size_t len);

/**
 * Cached accessor: computes the die ID once (thread-safe) and returns it.
 * Returns "" when no die ID is available.  Never NULL.
 */
const char *device_id_serial_cached(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_ID_H */
