/* PQTools `.bin` import/export for the Hi3516CV610 ISP.
 *
 * The vendor `libbin.so` carries no public header in the package we have, so
 * the call contract below was recovered from the stripped blob.  Every offset
 * and gate named in a comment here is a disassembly finding, not a guess from
 * a related SoC's header -- the older `HI_PQ_BIN` API on Hi3516CV100 has a
 * different shape and its error codes do not apply. */

#include "cv610_pq_bin.h"

#include "cv610_iq.h"
#include "cv610_pipeline.h"
#include "file_util.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ot_type.h"
#include "ss_mpi_sys.h"

#define PQ_LOG        "[pq] "
#define PQ_BIN_LIB    "libbin.so"
#define PQ_AE_LIB     "libss_mpi_ae.so"

/* The pipe cv610_iq.c drives; libbin reaches the 3DNR parameters through it. */
#define PQ_VI_PIPE    0

/* A whole ISP parameter image is ~140 KB.  The cap only exists so a wrong
 * path cannot turn into an unbounded allocation. */
#define PQ_BIN_MAX_BYTES (8u * 1024u * 1024u)

/* Smallest length we will believe from OT_PQ_GetISPDataTotalLen.  It answers 4
 * on an empty module table; a real image on this SDK is 143424. */
#define PQ_ISP_LEN_FLOOR 1024u

/* Allocation slack for export; see the comment at the calloc. */
#define PQ_EXPORT_SLACK 64u

/* Mirrors the fields libbin.so actually reads: OT_PQ_BIN_ImportBinData (@0xd9c)
 * reads the two enables at +0 and +4, and OT_PQ_BIN_ImportNRXData (@0x14e4)
 * passes +8 to PQ_BIN_SetPipeNRXParam as the VI pipe.  The vendor struct's tail
 * is unidentified, so the pad keeps anything past +8 reading as zero instead of
 * off the end of our allocation. */
typedef struct {
	int isp_enable;
	int nr_enable;
	int vi_pipe;
	int reserved[13];
} PqBinModule;

typedef struct {
	void *bin;
	void *ae;
	void *import_bin;      /* int  (*)(PqBinModule *, unsigned char *, unsigned int) */
	void *export_bin;      /* int  (*)(PqBinModule *, unsigned char *, unsigned int) */
	void *isp_total_len;   /* unsigned int (*)(void) */
	void *struct_param_len;/* unsigned int (*)(PqBinModule *) */
} PqBinLib;

typedef int (*pq_transfer_fn)(PqBinModule *, unsigned char *, unsigned int);
typedef unsigned int (*pq_isp_len_fn)(void);
typedef unsigned int (*pq_struct_len_fn)(PqBinModule *);

/* Storage for the AE library handle that libbin's g_aeHandle points AT.
 * Static because libbin dereferences the global long after pq_lib_open
 * returns. */
static void *g_ae_handle;

static void pq_lib_close(PqBinLib *lib)
{
	if (lib->bin)
		dlclose(lib->bin);
	if (lib->ae)
		dlclose(lib->ae);
	memset(lib, 0, sizeof(*lib));
}

/* libbin.so's DT_NEEDED lists only libc, but it carries undefined ss_mpi_*
 * symbols that the host process must already provide.  RTLD_NOW is deliberate:
 * a process that cannot satisfy them should fail here, with dlerror() naming
 * the symbol, rather than part-way through writing ISP registers. */
static int pq_lib_open(PqBinLib *lib)
{
	void **ae_slot;

	memset(lib, 0, sizeof(*lib));

	/* RTLD_LOCAL: every entry point is reached through dlsym() on this
	 * handle, so libbin never needs to export into the global scope -- and
	 * musl's dlclose is a no-op, so anything it did export would stay there
	 * for the life of the process, where it could satisfy a later plugin's
	 * lookup.  The undefined ss_mpi_* symbols it carries resolve from the
	 * executable and its DT_NEEDED libraries either way; that is what makes
	 * RTLD_NOW here a real check rather than a formality. */
	lib->bin = dlopen(PQ_BIN_LIB, RTLD_NOW | RTLD_LOCAL);
	if (!lib->bin) {
		fprintf(stderr, "WARNING: %sunable to load %s (%s)\n", PQ_LOG,
			PQ_BIN_LIB, dlerror());
		return -1;
	}

	lib->import_bin = dlsym(lib->bin, "OT_PQ_BIN_ImportBinData");
	lib->export_bin = dlsym(lib->bin, "OT_PQ_BIN_ExportBinData");
	lib->isp_total_len = dlsym(lib->bin, "OT_PQ_GetISPDataTotalLen");
	lib->struct_param_len = dlsym(lib->bin, "OT_PQ_GetStructParamLen");
	if (!lib->import_bin || !lib->export_bin || !lib->isp_total_len ||
		!lib->struct_param_len) {
		fprintf(stderr, "WARNING: %s%s is missing a PQ bin entry point\n",
			PQ_LOG, PQ_BIN_LIB);
		pq_lib_close(lib);
		return -1;
	}

	/* libbin resolves the AE MPI through this global rather than through a
	 * link-time dependency.  Left unset, the AE ext-register record -- one of
	 * the three the file carries -- has nowhere to land.
	 *
	 * The indirection is the trap: g_aeHandle is a `void **`, and libbin
	 * dereferences it TWICE (BIN_OpenDllFunc loads the global, then loads
	 * through it before calling dlsym).  Storing the dlopen handle directly
	 * makes it dlsym(*(void **)handle, ...) -- on musl that reads the first
	 * word of struct dso and resolves nothing, silently, while we print
	 * success.  So point the global at a variable that HOLDS the handle, and
	 * give that variable static lifetime: libbin reads it after this
	 * function returns. */
	lib->ae = dlopen(PQ_AE_LIB, RTLD_NOW | RTLD_LOCAL);
	ae_slot = (void **)dlsym(lib->bin, "g_aeHandle");
	if (!lib->ae || !ae_slot) {
		/* Clear it: a previous open may have left a handle there, and a
		 * stale one is worse than none. */
		if (ae_slot)
			*ae_slot = NULL;
		fprintf(stderr, "WARNING: %sno %s handle; AE parameters will not "
			"transfer\n", PQ_LOG, PQ_AE_LIB);
	} else {
		g_ae_handle = lib->ae;
		*ae_slot = &g_ae_handle;
	}

	return 0;
}

/* Both transfers refuse to run unless the ISP reports a tuning connection --
 * the same channel PQTools uses live.  We restore the previous value so a
 * one-shot import does not leave the ISP in tuning mode for the rest of the
 * session. */
static int pq_tuning_begin(td_s32 *saved)
{
	td_s32 connected = 0;

	if (ss_mpi_sys_get_tuning_connect(&connected) != TD_SUCCESS) {
		fprintf(stderr, "WARNING: %scannot read the tuning connect state\n",
			PQ_LOG);
		return -1;
	}
	*saved = connected;
	if (connected)
		return 0;

	if (ss_mpi_sys_set_tuning_connect(1) != TD_SUCCESS) {
		fprintf(stderr, "WARNING: %scannot enable the tuning connection\n",
			PQ_LOG);
		return -1;
	}
	return 0;
}

static void pq_tuning_end(td_s32 saved)
{
	if (!saved)
		(void)ss_mpi_sys_set_tuning_connect(0);
}

static unsigned char *pq_read_file(const char *path, size_t *out_len)
{
	unsigned char *buf;
	long size;
	FILE *f;

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "WARNING: %scannot open %s\n", PQ_LOG, path);
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
		fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "WARNING: %scannot size %s\n", PQ_LOG, path);
		fclose(f);
		return NULL;
	}
	if ((unsigned long)size > PQ_BIN_MAX_BYTES) {
		fprintf(stderr, "WARNING: %s%s is %ld bytes, over the %u byte cap\n",
			PQ_LOG, path, size, PQ_BIN_MAX_BYTES);
		fclose(f);
		return NULL;
	}

	buf = malloc((size_t)size);
	if (!buf) {
		fprintf(stderr, "WARNING: %scannot allocate %ld bytes for %s\n",
			PQ_LOG, size, path);
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "WARNING: %sshort read on %s\n", PQ_LOG, path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);

	*out_len = (size_t)size;
	return buf;
}

int cv610_pq_bin_import(const char *path)
{
	unsigned int isp_len, nrx_len, remainder;
	PqBinModule mod;
	PqBinLib lib;
	unsigned char *buf;
	td_s32 saved;
	size_t len = 0;
	int ret;

	if (!path || !*path)
		return 0;

	/* The same readiness gate every cv610_iq entry point makes.  Both call
	 * sites are after pipeline start today, so this cannot bite -- it is here
	 * because a module that writes ISP registers should not be the one that
	 * omits it. */
	if (!cv610_pipeline_isp_ready()) {
		fprintf(stderr, "WARNING: %sISP not running; %s not applied\n",
			PQ_LOG, path);
		return -1;
	}

	if (file_util_validate_regular_file(path, "PQ bin", PQ_LOG) != 0)
		return -1;

	if (pq_lib_open(&lib) != 0)
		return -1;

	buf = pq_read_file(path, &len);
	if (!buf) {
		pq_lib_close(&lib);
		return -1;
	}

	/* The file is [ISP image of exactly this length][optional NRX section].
	 * The length is a property of the running SDK, so asking the library is
	 * also the version check: a tune built against a different ISP generation
	 * does not have an ISP image of this size. */
	isp_len = ((pq_isp_len_fn)lib.isp_total_len)();
	/* OT_PQ_GetISPDataTotalLen returns 4 -- the record-count word alone --
	 * when its module table is empty, so "== 0" is not the degenerate case.
	 * Anything under a kilobyte is not an ISP image. */
	if (isp_len < PQ_ISP_LEN_FLOOR || len < isp_len) {
		fprintf(stderr, "WARNING: %s%s holds %zu bytes; this SDK's ISP image "
			"is %u -- wrong ISP version for this chip\n", PQ_LOG, path, len,
			isp_len);
		free(buf);
		pq_lib_close(&lib);
		return -1;
	}
	remainder = (unsigned int)(len - isp_len);

	memset(&mod, 0, sizeof(mod));
	mod.isp_enable = 1;
	mod.vi_pipe = PQ_VI_PIPE;

	/* Gate the 3DNR half on the section length the library itself expects.
	 * A header-size check is not enough: ImportNRXData does not forward our
	 * length to PQ_BIN_SetNRDataV2, which memcpy's a fixed ~1298-byte payload
	 * out of the buffer and only compares the declared size AFTERWARDS.  So a
	 * validly-headed but truncated file -- an interrupted scp, a full tmpfs --
	 * would read past the end of this allocation and push uninitialised heap
	 * into the 3DNR registers.  Asking the library for the size is the same
	 * authority we already trust for the ISP half.
	 *
	 * Skipping the section rather than letting the library refuse it is
	 * deliberate: its refusal is the return value of the WHOLE call and would
	 * mask an ISP half that landed. */
	nrx_len = ((pq_struct_len_fn)lib.struct_param_len)(&mod);
	mod.nr_enable = (nrx_len && remainder >= nrx_len) ? 1 : 0;
	if (remainder && !mod.nr_enable)
		fprintf(stderr, "WARNING: %s%s carries %u trailing bytes; the 3DNR "
			"section needs %u -- importing the ISP half only\n", PQ_LOG, path,
			remainder, nrx_len);

	if (pq_tuning_begin(&saved) != 0) {
		free(buf);
		pq_lib_close(&lib);
		return -1;
	}
	printf("> %sloading %s (%u B ISP + %u B 3DNR)\n", PQ_LOG, path, isp_len,
		mod.nr_enable ? nrx_len : 0);
	ret = ((pq_transfer_fn)lib.import_bin)(&mod, buf, (unsigned int)len);
	pq_tuning_end(saved);

	free(buf);
	pq_lib_close(&lib);

	if (ret != 0) {
		fprintf(stderr, "ERROR: %sOT_PQ_BIN_ImportBinData failed 0x%x for %s\n",
			PQ_LOG, (unsigned)ret, path);
		return -1;
	}
	/* The image carried an AE ext-register record, so this module's sibling
	 * must stop trusting the ceilings it latched at cold boot. */
	cv610_iq_forget_ae_defaults();
	printf("> %sISP tuning applied from %s\n", PQ_LOG, path);
	return 0;
}

int cv610_pq_bin_export(const char *path)
{
	unsigned int isp_len, nrx_len, need;
	PqBinModule mod;
	PqBinLib lib;
	unsigned char *buf;
	td_s32 saved;
	FILE *f;
	int ret;

	if (!path || !*path) {
		fprintf(stderr, "WARNING: %sexport needs a destination path\n", PQ_LOG);
		return -1;
	}

	if (pq_lib_open(&lib) != 0)
		return -1;

	memset(&mod, 0, sizeof(mod));
	mod.isp_enable = 1;
	mod.vi_pipe = PQ_VI_PIPE;

	isp_len = ((pq_isp_len_fn)lib.isp_total_len)();
	nrx_len = ((pq_struct_len_fn)lib.struct_param_len)(&mod);

	/* nr_enable is decided by the ANSWER, not asserted before the question.
	 * OT_PQ_GetStructParamLen returns 0 when the 3DNR query or its internal
	 * malloc fails, and ExportBinData recomputes the same sum and requires
	 * EXACT equality with our length -- so a hardcoded nr_enable=1 with a
	 * zero-length section passes that check and then writes the 37-byte NRX
	 * header at buf + isp_len, one past the end.  Asking for zero bytes of
	 * 3DNR is the one request the library will happily overrun. */
	mod.nr_enable = nrx_len ? 1 : 0;
	need = isp_len + (mod.nr_enable ? nrx_len : 0);
	if (isp_len < PQ_ISP_LEN_FLOOR || need < isp_len ||
		need > PQ_BIN_MAX_BYTES) {
		fprintf(stderr, "WARNING: %simplausible export size %u + %u\n", PQ_LOG,
			isp_len, nrx_len);
		pq_lib_close(&lib);
		return -1;
	}
	if (!mod.nr_enable)
		fprintf(stderr, "WARNING: %s3DNR parameters unavailable; exporting the "
			"ISP half only\n", PQ_LOG);

	/* Slack, but `need` is still what we pass as the length.  The vendor's
	 * two size formulas disagree with each other -- the writer branches on a
	 * different field value than the sizer does, and on a path this chip does
	 * not take it would emit up to 1410 bytes into a 1350-byte region.  The
	 * length argument must stay exact (the check is a != , not a <=), so the
	 * only place to absorb that is the allocation. */
	buf = calloc(1, (size_t)need + PQ_EXPORT_SLACK);
	if (!buf) {
		fprintf(stderr, "WARNING: %scannot allocate %u bytes for export\n",
			PQ_LOG, need);
		pq_lib_close(&lib);
		return -1;
	}

	if (pq_tuning_begin(&saved) != 0) {
		free(buf);
		pq_lib_close(&lib);
		return -1;
	}
	ret = ((pq_transfer_fn)lib.export_bin)(&mod, buf, need);
	pq_tuning_end(saved);
	pq_lib_close(&lib);

	if (ret != 0) {
		fprintf(stderr, "ERROR: %sOT_PQ_BIN_ExportBinData failed 0x%x\n",
			PQ_LOG, (unsigned)ret);
		free(buf);
		return -1;
	}

	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "ERROR: %scannot write %s\n", PQ_LOG, path);
		free(buf);
		return -1;
	}
	if (fwrite(buf, 1, need, f) != need) {
		fprintf(stderr, "ERROR: %sshort write on %s\n", PQ_LOG, path);
		fclose(f);
		free(buf);
		return -1;
	}
	if (fclose(f) != 0) {
		fprintf(stderr, "ERROR: %scannot flush %s\n", PQ_LOG, path);
		free(buf);
		return -1;
	}
	free(buf);

	printf("> %sISP state exported to %s (%u B)\n", PQ_LOG, path, need);
	return (int)need;
}
