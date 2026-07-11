/* maruko_stabfill_probe.c — Phase 5a bench (see maruko_stabfill_probe.h).
 *
 * Reverse-engineering bench, mirroring the MARUKO_DUAL_VENC_PROBE precedent in
 * maruko_pipeline.c.  Determines whether the i6c VENC accepts manually pushed
 * input frames (MI_SYS_ChnInputPortGetBuf/PutBuf) — the pivotal go/no-go for
 * porting Star6E's stab-fill compose path.  Env-gated; runs once; cleans up.
 *
 * The MI_SYS input-port frame/bufinfo layouts are the DEVICE-PROVEN i6c ABI
 * copied verbatim from src/maruko_framing_stab.c (StabFrame_t / StabBufInfo_t).
 * The BufConf layout is the VERIFIED i6c MI_SYS_BufConf_t — see the verdict
 * history below and in maruko_stabfill_probe.h: the original run's Star6E-
 * shaped conf + eBufType=0 (RAW on i6c) made its negative result an artifact.
 */
#include "maruko_stabfill_probe.h"
#include "maruko_mi.h"
#include "maruko_bindings.h"
#include "sigmastar_types.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── i6c MI_SYS input-port ABI ─────────────────────────────────────────────── */
typedef struct { MI_U16 x, y, w, h; } PrbRect_t;
typedef struct {                               /* == StabFrame_t (proven) */
	int eTileMode, ePixelFormat, eCompressMode, eFrameScanMode;
	int eFieldType, ePhylayoutType;
	MI_U16 u16Width, u16Height;
	void *pVirAddr[3];
	MI_U64 phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingStart, u16RingTotal;
	struct { int eType; union { MI_U32 g; } u; } stIsp;
	PrbRect_t stCrop;
} PrbFrame_t;
typedef struct {                               /* == StabBufInfo_t (proven) */
	MI_U64 u64Pts, u64Sideband;
	int eBufType;
	MI_U8 bEos, bUsrBuf;
	MI_U32 u32Seq;
	MI_U8 bDrop;
	union { PrbFrame_t stFrameData; MI_U8 pad[512]; };
	MI_U8 u8CusFlag;
} PrbBufInfo_t;
/* i6c MI_SYS_BufConf_t (mi_sys_datatype.h:503).  HISTORY: the original 5a run
 * used a Star6E-shaped layout (no bDirectBuf/bCrcCheck, extra-conf inside the
 * frame cfg) AND eBufType=0 — which is BUFDATA_RAW on i6c, not FRAME.  Both
 * errors made the pushes silently degenerate, so 5a's "VENC does not encode
 * manual pushes" device result was an ARTIFACT: with this corrected layout the
 * direct push encodes at the full sensor rate (the shipped stab-fill path). */
typedef struct {                               /* == MI_SYS_BufFrameConfig_t */
	MI_U16 u16Width, u16Height;
	int eFrameScanMode;
	int eFormat;
	int eCompressMode;
} PrbFrameCfg_t;
typedef struct {                               /* == MI_SYS_BufConf_t */
	int eBufType;                              /* offset 0 */
	MI_U32 u32Flags;                           /* offset 4 */
	MI_U64 u64TargetPts;                       /* offset 8 */
	MI_U8 bDirectBuf;                          /* offset 16 */
	MI_U8 bCrcCheck;                           /* offset 17 (pad → 24) */
	union {                                    /* offset 24, align 8 */
		PrbFrameCfg_t stFrameCfg;
		struct { MI_U32 u32Size; } stRawCfg;
		MI_U64 align8_;
		MI_U8 pad[64];
	};
} PrbBufConf_t;

#define PRB_BUFDATA_FRAME 1   /* E_MI_SYS_BUFDATA_FRAME (RAW=0 on i6c!) */

typedef MI_S32 (*prb_in_get_t)(i6_sys_bind *, PrbBufConf_t *, PrbBufInfo_t *,
	MI_S32 *, MI_S32);
typedef MI_S32 (*prb_in_put_t)(MI_S32, PrbBufInfo_t *, MI_U8);

/* MI_VENC_StartRecvPicEx arms continuous receive of PUSHED frames on i6c
 * (s32RecvPicNum = -1 = unlimited); plain StartRecvPic can be RING-only. */
typedef struct { MI_S32 s32RecvPicNum; } PrbRecvParam_t;
typedef MI_S32 (*prb_start_recv_ex_t)(int, int, PrbRecvParam_t *);
static prb_start_recv_ex_t g_start_recv_ex;   /* NULL -> fall back to StartRecvPic */

/* Probe one src-conf mode on a fresh VENC channel.  Returns:
 *   1  = manual push accepted AND VENC emitted a stream (WORKS)
 *   0  = channel ran but no stream came out (push not encoded)
 *  -1  = the mode/channel could not be set up (rejected / error) */
static int probe_mode(int dev, const i6c_venc_chn *chn0_attr, int probe_chn,
	i6c_venc_src_conf mode, const char *name, uint32_t w, uint32_t h,
	prb_in_get_t in_get, prb_in_put_t in_put)
{
	i6c_venc_chn attr = *chn0_attr;   /* reuse channel-0 encode config */
	i6_sys_bind port;
	i6c_venc_src_conf m = mode;
	int pushed = 0, get_fail = 0, first_get_ret = 0;
	int emitted_packs = 0;
	MI_S32 ret;
	int i, t;

	printf("> [5a] ===== mode %s (src_conf=%d) chn=%d =====\n",
		name, (int)mode, probe_chn);

	ret = maruko_mi_venc_create_chn(dev, probe_chn, &attr);
	if (ret != 0) {
		printf("> [5a] %s CreateChn ret=%d -> cannot test this mode\n",
			name, (int)ret);
		return -1;
	}

	ret = maruko_mi_venc_set_input_source(dev, probe_chn, &m);
	printf("> [5a] %s SetInputSourceConfig(%s) ret=%d\n", name, name, (int)ret);
	if (ret != 0) {
		(void)maruko_mi_venc_destroy_chn(dev, probe_chn);
		printf("> [5a] %s -> src-conf REJECTED (not a manual-push mode)\n", name);
		return -1;
	}

	if (g_start_recv_ex) {
		PrbRecvParam_t rp = { -1 };   /* unlimited continuous receive */
		ret = g_start_recv_ex(dev, probe_chn, &rp);
		printf("> [5a] %s StartRecvPicEx(-1) ret=%d\n", name, (int)ret);
	} else {
		ret = maruko_mi_venc_start_recv(dev, probe_chn);
		printf("> [5a] %s StartRecvPic ret=%d\n", name, (int)ret);
	}
	if (ret != 0) {
		(void)maruko_mi_venc_destroy_chn(dev, probe_chn);
		return -1;
	}

	memset(&port, 0, sizeof(port));
	port.module  = I6_SYS_MOD_VENC;
	port.device  = (unsigned)dev;
	port.channel = (unsigned)probe_chn;
	port.port    = 0;

	/* Push ~12 hand-filled gray frames. */
	for (i = 0; i < 12; i++) {
		PrbBufConf_t conf;
		PrbBufInfo_t buf;
		PrbFrame_t *fd;
		MI_S32 handle = 0;

		memset(&conf, 0, sizeof(conf));
		conf.eBufType = PRB_BUFDATA_FRAME;
		conf.u64TargetPts = (MI_U64)i * 20000;   /* ~50 fps */
		conf.stFrameCfg.u16Width  = (MI_U16)w;
		conf.stFrameCfg.u16Height = (MI_U16)h;
		conf.stFrameCfg.eFormat   = I6_PIXFMT_YUV420SP;  /* 0 */
		conf.stFrameCfg.eFrameScanMode = 0;              /* PROGRESSIVE */

		memset(&buf, 0, sizeof(buf));
		ret = in_get(&port, &conf, &buf, &handle, 100);
		if (ret != 0) {
			if (i == 0) first_get_ret = (int)ret;
			get_fail++;
			usleep(20000);
			continue;
		}
		fd = &buf.stFrameData;
		/* Gray fill (Y=128, UV=128) over the mapped planes. */
		if (fd->pVirAddr[0] && fd->u32Stride[0])
			memset(fd->pVirAddr[0], 128,
				(size_t)fd->u32Stride[0] * fd->u16Height);
		if (fd->pVirAddr[1] && fd->u32Stride[1])
			memset(fd->pVirAddr[1], 128,
				(size_t)fd->u32Stride[1] * (fd->u16Height / 2));

		ret = in_put(handle, &buf, 0);   /* 0 = submit to VENC */
		if (ret == 0) pushed++;
		else if (pushed == 0)
			printf("> [5a] %s ChnInputPortPutBuf ret=%d\n", name, (int)ret);
		usleep(20000);
	}
	printf("> [5a] %s pushed=%d get_fail=%d (first GetBuf ret=%d)\n",
		name, pushed, get_fail, first_get_ret);

	/* Drain any encoded output. */
	for (t = 0; t < 25 && emitted_packs == 0; t++) {
		i6_venc_stat stat;
		memset(&stat, 0, sizeof(stat));
		if (maruko_mi_venc_query(dev, probe_chn, &stat) == 0 &&
		    stat.curPacks > 0) {
			i6c_venc_strm strm;
			i6c_venc_pack packs[8];
			memset(&strm, 0, sizeof(strm));
			memset(packs, 0, sizeof(packs));
			strm.packet = packs;
			strm.count  = stat.curPacks > 8 ? 8 : stat.curPacks;
			if (maruko_mi_venc_get_stream(dev, probe_chn, &strm, 40) == 0) {
				emitted_packs += (int)strm.count;
				(void)maruko_mi_venc_release_stream(dev, probe_chn, &strm);
			}
		}
		usleep(10000);
	}

	printf("> [5a] %s RESULT: emitted_packs=%d => %s\n", name, emitted_packs,
		emitted_packs > 0 ? "*** MANUAL PUSH WORKS ***"
		: (pushed > 0 ? "pushed but NO encoded output"
		             : "could not push frames"));

	(void)maruko_mi_venc_stop_recv(dev, probe_chn);
	(void)maruko_mi_venc_destroy_chn(dev, probe_chn);
	return emitted_packs > 0 ? 1 : (pushed > 0 ? 0 : -1);
}

void maruko_stabfill_probe_run(int venc_dev, const void *chn0_attr,
	uint32_t enc_w, uint32_t enc_h)
{
	static const struct { i6c_venc_src_conf mode; const char *name; } cands[] = {
		{ I6C_VENC_SRC_CONF_NORMAL,  "NORMAL"  },
		{ I6C_VENC_SRC_CONF_HW_SYNC, "HW_SYNC" },
	};
	void *sys, *venc;
	prb_in_get_t in_get;
	prb_in_put_t in_put;
	int any_works = 0, n, chn;

	printf("\n> [5a] ===== Maruko stab-fill manual-push probe (dev=%d %ux%u) =====\n",
		venc_dev, enc_w, enc_h);

	sys = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!sys) {
		printf("> [5a] dlopen libmi_sys: %s — ABORT\n", dlerror());
		return;
	}
	in_get = (prb_in_get_t)dlsym(sys, "MI_SYS_ChnInputPortGetBuf");
	in_put = (prb_in_put_t)dlsym(sys, "MI_SYS_ChnInputPortPutBuf");
	if (!in_get || !in_put) {
		printf("> [5a] MI_SYS_ChnInputPort{Get,Put}Buf missing (get=%p put=%p)"
			" — ABORT\n", (void *)in_get, (void *)in_put);
		return;
	}
	/* StartRecvPicEx(-1) to arm continuous receive of pushed frames (the main
	 * pipeline uses plain StartRecvPic, which may be RING-only). */
	venc = dlopen("libmi_venc.so", RTLD_LAZY | RTLD_GLOBAL);
	g_start_recv_ex = venc ?
		(prb_start_recv_ex_t)dlsym(venc, "MI_VENC_StartRecvPicEx") : NULL;
	printf("> [5a] input-port symbols resolved; StartRecvPicEx=%s; probing %d mode(s)\n",
		g_start_recv_ex ? "yes" : "no(fallback StartRecvPic)",
		(int)(sizeof(cands) / sizeof(cands[0])));

	for (n = 0; n < (int)(sizeof(cands) / sizeof(cands[0])); n++) {
		chn = 4 + n;   /* distinct from ch0 (main) and ch1 (dual-venc probe) */
		int r = probe_mode(venc_dev, (const i6c_venc_chn *)chn0_attr, chn,
			cands[n].mode, cands[n].name, enc_w, enc_h, in_get, in_put);
		if (r == 1) any_works = 1;
	}

	printf("> [5a] ===== VERDICT: %s =====\n\n", any_works
		? "i6c VENC ACCEPTS MANUAL PUSH — stab-fill port is viable (Phase 5b+)"
		: "i6c VENC does NOT emit on manual push — fall back to DIVP/RGN compose");
}
