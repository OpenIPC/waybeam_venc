/*
 * ipu_probe — Star6E IPU (NPU) bring-up tool.
 *
 * Phase-1 hardware gate for the NPU detection feature (see
 * specs/2026-07-17-star6e-npu-detect/).  DEV-ONLY: cross-compiled for the
 * target, not part of the shipped waybeam binary.  Run it on a Star6E board
 * with libmi_ipu.so present:
 *
 *     make ipu-probe
 *     scp out/star6e/ipu_probe root@<board>:/tmp/
 *     ssh root@<board> /tmp/ipu_probe <firmware.bin> <network.img> [input.nv12]
 *
 * It loads the IPU, creates a device + channel from the given firmware and
 * compiled-network blobs, prints the input/output tensor descriptions, and —
 * if an NV12 file is supplied — runs one Invoke and dumps output-tensor
 * statistics (dequantized min/max/mean + a few samples).
 *
 * The point is to VERIFY the ABI assumptions in star6e_ipu.h against real
 * silicon: tensor shapes, formats, and the dequant scale/zero-point that
 * YOLOv8 decode depends on.  Nothing here is load-bearing for production.
 */

#include "star6e_ipu.h"
#include "detect_dequant.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ── IPU private MMA pool ───────────────────────────────────────────────
 * libmi_ipu allocates its firmware/model/IO buffers from a per-device
 * private pool of the IPU device (module 29).  Nothing configures that
 * pool on an OpenIPC system ("fail to get IPU MMA heap name"), so carve
 * one out of the default bootarg heap before CreateDevice.  Struct layout
 * mirrors MI_SYS_GlobalPrivPoolConfig_t (mi_sys_datatype.h). */
#define IPU_MODULE_ID           29u
#define PER_DEV_PRIVATE_POOL    2u
#define IPU_POOL_HEAP           "mma_heap_name0"
#define IPU_POOL_SIZE           (24u * 1024u * 1024u)

struct probe_dev_pool_conf {
	unsigned int  module_id;
	unsigned int  dev_id;
	unsigned int  reserve;
	unsigned char heap_name[32];
	unsigned int  heap_size;
};

struct probe_pool_config {
	unsigned int config_type;
	unsigned char create;         /* MI_BOOL */
	union {
		struct probe_dev_pool_conf dev;
		unsigned char pad[56];
	} u;
};

/* ── model-blob reader ──────────────────────────────────────────────────
 * The SDK forwards the path (fw_path / net_path) as the readFunc `ctx`.
 * We open it lazily and cache one FILE* keyed by the path pointer so a
 * multi-chunk load doesn't reopen per call.  A real integration would feed
 * decrypted bytes from memory here instead.
 *
 * Retained but currently unused: the MI_IPU calls below mirror the vendor
 * samples, which pass a NULL read-callback and let libmi_ipu open the blob
 * path itself.  This callback is the seam the Phase-5 decrypt path will use
 * (pass it in place of NULL to stream plaintext from memory). */
static FILE *g_blob_fp;
static char *g_blob_path;

static int __attribute__((unused)) blob_read(void *data, int offset, int size,
	char *ctx)
{
	if (ctx != g_blob_path) {
		if (g_blob_fp)
			fclose(g_blob_fp);
		g_blob_fp = fopen(ctx, "rb");
		g_blob_path = ctx;
		if (!g_blob_fp) {
			fprintf(stderr, "[probe] cannot open blob %s\n", ctx);
			return -1;
		}
	}
	if (fseek(g_blob_fp, offset, SEEK_SET) != 0)
		return -1;
	return (int)fread(data, 1, (size_t)size, g_blob_fp);
}

static long file_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	n = ftell(f);
	fclose(f);
	return n;
}

static void print_tensor(const char *tag, unsigned int i, const IpuTensorDesc *t)
{
	unsigned int d;
	printf("  %s[%u] name=\"%s\" fmt=%s dim=%u shape=[", tag, i, t->name,
		star6e_ipu_fmt_name(t->format), t->dimension);
	for (d = 0; d < t->dimension && d < 8; d++)
		printf("%s%u", d ? "," : "", t->shape[d]);
	printf("] scalar=%g zero=%lld aligned_buf=%d\n",
		(double)t->scalar, t->zero_pnt, t->aligned_buf_size);
}

/* Source bytes per element for the dequantizable formats (0 = unsupported),
 * mirroring the element sizes detect_dequant_buffer reads. */
static size_t ipu_fmt_elem_size(IpuFmt fmt)
{
	switch (fmt) {
	case IPU_FMT_U8:
	case IPU_FMT_INT8:  return 1;
	case IPU_FMT_INT16: return 2;
	case IPU_FMT_INT32:
	case IPU_FMT_FP32:  return 4;
	default:            return 0;
	}
}

/* Dump dequantized stats for one output tensor. */
static void dump_output_stats(const IpuTensorDesc *desc, const IpuTensor *ten)
{
	size_t count = 1;
	unsigned int d;
	float *vals;
	size_t i, esz;
	float mn, mx, sum;

	for (d = 0; d < desc->dimension && d < 8; d++) {
		/* A malformed .img can carry a garbage shape; guard the running
		 * product so count*elemsize below cannot wrap on 32-bit size_t
		 * (short alloc -> heap overflow) or over-read the source. */
		if (desc->shape[d] != 0 &&
		    count > SIZE_MAX / desc->shape[d]) {
			printf("    (tensor shape overflows)\n");
			return;
		}
		count *= desc->shape[d];
	}
	if (count == 0 || !ten->data[0]) {
		printf("    (no data)\n");
		return;
	}
	esz = ipu_fmt_elem_size(desc->format);
	if (esz == 0) {
		printf("    (format %s not dequantizable)\n",
			star6e_ipu_fmt_name(desc->format));
		return;
	}
	if (count > SIZE_MAX / sizeof(*vals) ||
	    (desc->aligned_buf_size > 0 &&
	     count > (size_t)desc->aligned_buf_size / esz)) {
		printf("    (tensor %zu elems exceeds source buffer)\n", count);
		return;
	}

	vals = malloc(count * sizeof(*vals));
	if (!vals) {
		printf("    (alloc failed for %zu elems)\n", count);
		return;
	}
	if (detect_dequant_buffer(ten->data[0], desc->format, desc->scalar,
		desc->zero_pnt, vals, count) != 0) {
		printf("    (format %s not dequantizable)\n",
			star6e_ipu_fmt_name(desc->format));
		free(vals);
		return;
	}

	mn = mx = vals[0];
	sum = 0.0f;
	for (i = 0; i < count; i++) {
		if (vals[i] < mn) mn = vals[i];
		if (vals[i] > mx) mx = vals[i];
		sum += vals[i];
	}
	printf("    dequant: count=%zu min=%g max=%g mean=%g  first=[",
		count, (double)mn, (double)mx, (double)(sum / (float)count));
	for (i = 0; i < count && i < 6; i++)
		printf("%s%g", i ? "," : "", (double)vals[i]);
	printf("]\n");
	free(vals);
}

int main(int argc, char **argv)
{
	const char *fw_path;
	const char *net_path;
	const char *nv12_path = NULL;
	long fw_size;
	IpuDevAttr dev;
	IpuOfflineInfo offline;
	IpuChnAttr chn_cfg;
	IpuTensorIODesc io;
	unsigned int channel = 0;
	unsigned int i;
	int rc = 1;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s <firmware.bin> <network.img> [input.nv12]\n",
			argv[0]);
		return 2;
	}
	fw_path = argv[1];
	net_path = argv[2];
	if (argc >= 4)
		nv12_path = argv[3];

	fw_size = file_size(fw_path);
	if (fw_size < 0) {
		fprintf(stderr, "[probe] cannot stat firmware %s\n", fw_path);
		return 2;
	}

	if (star6e_ipu_load() != 0) {
		fprintf(stderr, "[probe] IPU library load failed\n");
		return 1;
	}
	printf("[probe] libmi_ipu.so loaded\n");

	if (g_mi_ipu.fnSysInit && g_mi_ipu.fnSysInit(0) != 0) {
		fprintf(stderr, "[probe] MI_SYS_Init failed\n");
		goto out_unload;
	}
	printf("[probe] MI_SYS initialized\n");


	memset(&offline, 0, sizeof(offline));
	if (g_mi_ipu.fnGetOfflineInfo(NULL, (char *)net_path, &offline) != 0) {
		fprintf(stderr, "[probe] GetOfflineModeStaticInfo failed\n");
		goto out_unload;
	}
	printf("[probe] model %s: var_buf=%u model_size=%u\n",
		net_path, offline.var_buf_size, offline.model_size);

	memset(&dev, 0, sizeof(dev));
	dev.max_dyn_buf_size = offline.var_buf_size;
	dev.yuv420_walign = 16;
	dev.yuv420_halign = 2;
	dev.rgb_walign = 16;
	if (g_mi_ipu.fnCreateDevice(&dev, NULL, (char *)fw_path, 0) != 0) {
		fprintf(stderr, "[probe] CreateDevice failed\n");
		goto out_unload;
	}
	printf("[probe] device created (fw=%s, %ld bytes)\n", fw_path, fw_size);

	memset(&chn_cfg, 0, sizeof(chn_cfg));
	chn_cfg.usr_depth = 2;
	chn_cfg.buf_depth = 2;
	if (g_mi_ipu.fnCreateChannel(&channel, &chn_cfg, NULL,
		(char *)net_path) != 0) {
		fprintf(stderr, "[probe] CreateChannel failed\n");
		goto out_dev;
	}
	printf("[probe] channel %u created (net=%s)\n", channel, net_path);

	memset(&io, 0, sizeof(io));
	if (g_mi_ipu.fnGetIODesc(channel, &io) != 0) {
		fprintf(stderr, "[probe] GetInOutTensorDesc failed\n");
		goto out_chn;
	}
	printf("[probe] tensors: %u in, %u out\n", io.in_count, io.out_count);
	for (i = 0; i < io.in_count && i < IPU_MAX_TENSORS; i++)
		print_tensor("in", i, &io.in_desc[i]);
	for (i = 0; i < io.out_count && i < IPU_MAX_TENSORS; i++)
		print_tensor("out", i, &io.out_desc[i]);

	if (nv12_path) {
		IpuTensorVector ins, outs;
		long want, got;

		memset(&ins, 0, sizeof(ins));
		memset(&outs, 0, sizeof(outs));
		if (g_mi_ipu.fnGetInputTensors(channel, &ins) != 0 ||
		    ins.count == 0 || !ins.tensor[0].data[0]) {
			fprintf(stderr, "[probe] GetInputTensors failed\n");
			goto out_chn;
		}
		want = io.in_desc[0].aligned_buf_size;
		got = 0;
		{
			FILE *f = fopen(nv12_path, "rb");
			if (!f) {
				fprintf(stderr, "[probe] cannot open %s\n", nv12_path);
				goto out_chn;
			}
			got = (long)fread(ins.tensor[0].data[0], 1,
				want > 0 ? (size_t)want : 0, f);
			fclose(f);
		}
		printf("[probe] loaded %ld/%ld input bytes from %s\n",
			got, want, nv12_path);

		if (g_mi_ipu.fnPutInputTensors(channel, &ins) != 0) {
			fprintf(stderr, "[probe] PutInputTensors failed\n");
			goto out_chn;
		}
		if (g_mi_ipu.fnInvoke(channel, &ins, &outs) != 0) {
			fprintf(stderr, "[probe] Invoke failed\n");
			goto out_chn;
		}
		printf("[probe] Invoke OK, %u output tensors\n", outs.count);
		for (i = 0; i < outs.count && i < io.out_count; i++) {
			print_tensor("out", i, &io.out_desc[i]);
			dump_output_stats(&io.out_desc[i], &outs.tensor[i]);
		}
	}

	rc = 0;

out_chn:
	g_mi_ipu.fnDestroyChannel(channel);
out_dev:
	g_mi_ipu.fnDestroyDevice();
out_unload:
	if (g_blob_fp)
		fclose(g_blob_fp);
	star6e_ipu_unload();
	printf("[probe] done (rc=%d)\n", rc);
	return rc;
}
