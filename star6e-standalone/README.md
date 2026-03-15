# Standalone Encoder (Star6E + Maruko)

This folder contains the standalone encoder/streamer source code.

See the [root README](../README.md) for build instructions, HTTP API
documentation, configuration reference, and usage examples.

## Build Targets

- `SOC_BUILD=star6e` — Star6E (Infinity6E) backend
- `SOC_BUILD=maruko` — Maruko (Infinity6C) backend

## Directory Layout

- `src/` — Implementation source files
- `include/` — Public headers
- `tests/` — Host test suite (run with `make test-ci`)
- `tools/` — Host-native utilities (`rtp_timing_probe`)
- `config/` — Default configuration templates
- `scripts/` — Remote test and API test helpers
- `libs/` — Bundled runtime libraries per backend
