# JSON Config And HTTP API Roadmap

## Goal
- Replace ad-hoc CLI option parsing with a single JSON configuration source.
- Keep runtime invocation minimal:
  - `venc` (loads `/etc/venc.json`)
- Keep the local HTTP control API for live setting updates coherent with the config model.
- Keep API behavior versioned and documented in:
  - `documentation/HTTP_API_CONTRACT.md`

## Current State
- JSON runtime wiring is active in the current codebase.
- The primary runtime path is `/etc/venc.json` plus the HTTP API.
- Historical rollback postmortem and guardrails remain in:
  - `documentation/JSON_CONFIG_ROLLBACK_NOTES.md`

## Config Model
- Default config path:
  - `/etc/venc.json`
- In-repo default template:
  - `config/venc.default.json`
- Schema versioning:
  - include `schema_version` in config,
  - reject unknown schema versions,
  - allow optional forward-compatible keys only when explicitly enabled.
- Sensor selection model:
  - user sets only `video.capture_resolution` + `video.fps`,
  - backend auto-selects matching sensor mode,
  - invalid combinations fail fast (no silent fps fallback).

## Settings Inventory (Comprehensive)

### Core startup identity
- `backend.soc_build`
- `backend.strict_validation`

### Stream output
- `stream.mode` (`rtp`, `compact`)
- `stream.host`
- `stream.port`
- `stream.max_payload_size`

### Video/encoder RC
- `video.codec` (`264*`, `265*` modes)
- `video.bitrate_kbps`
- `video.fps`
- `video.gop_denominator`
- `video.output_resolution`
- `video.capture_resolution`
- `video.precrop` (planned)
- `video.version_profile`

Planned `video.precrop` intent:
- Pre-crop sensor frames before scaling so output keeps the requested aspect ratio.
- Primary mode should be simple and user-facing:
  - `off`: no pre-crop,
  - `auto`: center-crop capture frame to output aspect ratio before resize.
- Validation rule:
  - if requested crop/aspect cannot be satisfied from selected capture mode, fail fast.

### Encoder behavior
- `encoder.roi_enabled`
- `encoder.roi_qp`
- `encoder.by_frame`

### Image controls
- `image.mirror`
- `image.flip`
- `image.rotate_180`
- `image.limit_exposure`

### Sensor control
- `sensor.unlock.enabled`
- `sensor.unlock.cmd`
- `sensor.unlock.register`
- `sensor.unlock.value`
- `sensor.unlock.direction`

### ISP
- `isp.bin_path`

### Future runtime control namespace
- `future.http_api.enabled`
- `future.http_api.bind`
- `future.http_api.port`

## Mutability Classes (HTTP API)

### Live apply (no stream restart target)
- bitrate (`video.bitrate_kbps`)
- GOP denominator (`video.gop_denominator`) if backend API supports safe update
- ROI enable/qp
- image mirror/flip where backend supports runtime param update

### Requires graph rebind or channel restart
- codec mode (`video.codec`)
- resolution (`video.output_resolution`, `video.capture_resolution`)
- pre-crop behavior (`video.precrop`)
- stream packetization mode (`stream.mode`)
- low-delay pipeline mode changes (future)

### Requires full pipeline restart
- sensor capture resolution/fps mode re-selection
- sensor unlock custom-command settings
- ISP bin path changes (unless backend gains safe hot-reload semantics)

## Proposed HTTP Endpoints (v1)
- `GET /api/v1/version`
  - app version, schema version, backend target
- `GET /api/v1/config`
  - full active config
- `PUT /api/v1/config`
  - replace config, validate, apply with mutability rules
- `PATCH /api/v1/config`
  - partial update, same validation/apply model
- `GET /api/v1/capabilities`
  - backend-supported runtime fields + mutability class
- `POST /api/v1/actions/restart`
  - explicit controlled pipeline restart

Contract requirement:
- Endpoint definitions, payload schemas, status codes, and mutability semantics
  are normative in `documentation/HTTP_API_CONTRACT.md`.
- Every HTTP behavior change must update the contract in the same PR.
- Endpoint/payload design must follow contract principles:
  - lean endpoints with direct value,
  - simple descriptive JSON payloads.

## Backend Strategy
- Star6E-first rule for SigmaStar API-touching behavior:
  - implement and validate behavior on Star6E,
  - then port to Maruko.
- Shared architecture (JSON loader, validator, HTTP server framework):
  - implement for both backends in the same phase.
- Maruko can return explicit `not_implemented` on per-setting apply calls
  until parity is complete.

## Implementation Phases
1. Completed foundation:
   - JSON runtime integration in the main startup path.
   - HTTP server with read-only and write/apply endpoints.
   - live vs restart-required setting classes wired into the runtime.
2. Next:
   - harden schema validation and document strict vs compatible handling.
   - keep `/etc/venc.json`, helper scripts, and HTTP contract examples synchronized.
3. Then:
   - expand automated coverage for live-field, restart-required, and error-path behavior.
4. Then:
   - tighten Star6E and Maruko parity checks around config/runtime mutations.
5. Then:
   - extend transactional apply/rollback where richer multi-field changes need it
     (including `video.precrop` and more complex output reconfiguration).
