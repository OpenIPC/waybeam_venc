#ifndef DETECT_PLUGIN_H
#define DETECT_PLUGIN_H

/*
 * detect_plugin.h — the host <-> detector-backend contract.
 *
 * The venc host owns the model-independent half of the NPU detection path
 * (the VPE tap, the reader thread, the OSD overlay, config, and the RTP
 * sidecar carrier).  A *backend* owns the model-specific half: the IPU
 * device lifecycle and the tensor decode.  A backend lives in an external,
 * dlopen()'d plugin `.so` named by config (`detect.plugin`), so a new model or
 * detector family is a config change, not a rebuild — and, crucially, all
 * model-specific code (decode, class tables, schemas) stays OUT of this
 * (public) repo.  The public host knows only this ABI; the plugins live in a
 * separate private repo and ship as prebuilt `.so`s.
 *
 * A plugin exports one symbol, WAYBEAM_DETECT_ENTRY (below), returning a
 * DetectBackend.  The host dlopens the plugin, resolves that symbol, checks
 * `abi`, and drives the returned vtable.
 *
 * ABI stability: DetectBox and DetectFrame are laid out so a plugin wrapping a
 * prebuilt worker (whose structs are det = 24 B, frame = 32 B) can pass arrays
 * straight through with no per-element translation.  Do not reorder these
 * fields without bumping DETECT_PLUGIN_ABI.
 */

#include <stdint.h>

#define DETECT_PLUGIN_ABI 1u

/* One detection.  Box coords are FP32 PIXELS in the model's NETWORK space
 * (e.g. 640x352), corner form.  24-byte layout (see ABI note above). */
typedef struct {
	float   x1;      /* left   (net px) */
	float   y1;      /* top    (net px) */
	float   x2;      /* right  (net px) */
	float   y2;      /* bottom (net px) */
	float   score;   /* class probability (already sigmoid'd by the model) */
	int32_t cls;     /* class id */
} DetectBox;

/* The NV12 frame the host hands a backend, by physical address (zero-copy:
 * the IPU DMA reads the VPE buffer directly).  Matches ipu_yolo_frame_t
 * (sizeof 32).  A backend may require exact dims/strides and reject
 * mismatches. */
typedef struct {
	uint32_t width;      /* luma width  */
	uint32_t height;     /* luma height */
	uint32_t stride_y;   /* Y  plane stride */
	uint32_t stride_uv;  /* UV plane stride */
	uint64_t phy_y;      /* phys addr of Y  plane */
	uint64_t phy_uv;     /* phys addr of UV plane */
} DetectFrame;

/* Backend configuration passed to init().  Empty/NULL string or a
 * non-positive number means "use the plugin's built-in default". */
typedef struct {
	const char *model_path;      /* SigmaStar offline .img */
	const char *firmware_path;   /* IPU firmware (may be NULL) */
	int         display_w;       /* for any internal overlay scaling */
	int         display_h;
	float       conf_thresh;     /* <=0 -> plugin default */
	float       nms_iou;         /* <=0 -> plugin default */
} DetectBackendConfig;

/*
 * A detector backend.  All calls operate on a single backend instance
 * (the reference "worker" backend is a stateless singleton, so these carry
 * no handle — one detector per process).
 *
 *  init(cfg)                 0 on success, <0 on failure.
 *  process(frame,out,max,n)  run one inference; write up to `max` boxes into
 *                            `out`, set *n to the count (n may be NULL).
 *                            0 on success, <0 on error.
 *  set_display(w,h)          update overlay scaling target (may be a no-op).
 *  deinit()                  tear down the IPU/model/overlay.
 *  describe(&names)          optional; set *names to a static array of class
 *                            label strings and return the class count, or 0 /
 *                            leave the field pointer NULL if not provided.
 */
typedef struct DetectBackend {
	const char *name;
	uint32_t    abi;
	int  (*init)(const DetectBackendConfig *cfg);
	int  (*process)(const DetectFrame *frame, DetectBox *out, int max,
	                int *count);
	void (*set_display)(int width, int height);
	void (*deinit)(void);
	int  (*describe)(const char *const **class_names);
} DetectBackend;

/*
 * Plugin entry point.  Every detector plugin `.so` exports one function with
 * this name and signature; it returns a static, non-owning DetectBackend
 * (or NULL if the plugin cannot provide one).  The host dlopens the plugin,
 * resolves this symbol, verifies backend->abi == DETECT_PLUGIN_ABI, then
 * drives the vtable.
 *
 *   const DetectBackend *waybeam_detect_entry(void);
 */
#define WAYBEAM_DETECT_ENTRY "waybeam_detect_entry"
typedef const DetectBackend *(*WaybeamDetectEntryFn)(void);

#endif /* DETECT_PLUGIN_H */
