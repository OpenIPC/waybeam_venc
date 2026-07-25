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

/*
 * ABI version.  The host requires an EXACT match and rejects anything else —
 * during beta there is no compatibility window, so there is deliberately no
 * "older plugin, reduced checking" mode: every accepted plugin supports the
 * full vtable below.  Bump this whenever the vtable or a struct layout
 * changes, and rebuild the plugins.
 *
 *   1  initial: init / process / set_display / deinit / describe.
 *   2  adds model_dims() plus the net_width/net_height config fields, so the
 *      host verifies the tap geometry against the model's real input dims
 *      rather than trusting config.
 *   3  adds set_thresholds(), so conf/iou can change without rebuilding the
 *      NPU graph.  Numbered separately from 2 even though both landed in the
 *      same unmerged change: a vtable field was appended, so a host built
 *      against 3 reading set_thresholds on a plugin built against 2 would read
 *      past that plugin's struct.  The version bump turns that into a clean
 *      refusal instead — which is the whole point of the field.
 *
 * Note for any future compatibility window: `abi` bounds how much of
 * DetectBackend the host may read, because the PLUGIN owns that struct (it
 * returns a static one, sized by the header it compiled against) — reading a
 * field its ABI predates is out of bounds.  DetectBackendConfig is the other
 * direction (host-allocated, passed by pointer), so appending fields there is
 * always safe.
 */
#define DETECT_PLUGIN_ABI 3u

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
	/* ABI 2+: the tap geometry frames will arrive at.  A backend MAY refuse
	 * init() on a mismatch with its model; the host checks independently via
	 * model_dims(), so a backend that ignores these is still covered. */
	uint32_t    net_width;
	uint32_t    net_height;
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
 *  model_dims(&w,&h)         REQUIRED.  Report the loaded model's REAL input
 *                            geometry (what the compiled .img expects), not
 *                            what config asked for.  Valid only after a
 *                            successful init(); 0 on success, <0 if unknown.
 *                            The host compares this against the tap it created
 *                            and refuses a mismatch — the only way to catch
 *                            "operator pointed model_path at a
 *                            different-geometry .img", since config is not
 *                            evidence of what the model wants.
 *  set_thresholds(conf,iou)  optional.  Update the decode thresholds in place,
 *                            without reloading the model.  Both are decode-time
 *                            knobs, so rebuilding the NPU graph to change one
 *                            costs hundreds of ms to seconds of frame output
 *                            for nothing.  A NULL here is legal — the host then
 *                            falls back to a full reload, which is correct but
 *                            slow.  Called only while the host has quiesced the
 *                            frame consumer, so it need not be thread-safe.
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
	int  (*model_dims)(uint32_t *width, uint32_t *height);    /* ABI 2, required */
	void (*set_thresholds)(float conf_thresh, float nms_iou); /* ABI 3, optional */
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
