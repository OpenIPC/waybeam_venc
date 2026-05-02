#include "audio_codec.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int audio_codec_parse_name(const char *name)
{
	if (!name || !*name)
		return AUDIO_CODEC_TYPE_RAW;
	if (strcmp(name, "g711a") == 0)
		return AUDIO_CODEC_TYPE_G711A;
	if (strcmp(name, "g711u") == 0)
		return AUDIO_CODEC_TYPE_G711U;
	if (strcmp(name, "opus") == 0)
		return AUDIO_CODEC_TYPE_OPUS;
	return AUDIO_CODEC_TYPE_RAW;
}

static uint8_t pcm16_to_alaw(int16_t pcm_in)
{
	int pcm = pcm_in;
	int sign = 0;
	int exponent;
	int mantissa;

	if (pcm < 0) {
		pcm = -pcm - 1;
		sign = 0x80;
	}
	if (pcm > 32635)
		pcm = 32635;

	if (pcm >= 256) {
		exponent = 7;
		for (int exp_mask = 0x4000; !(pcm & exp_mask) && exponent > 1;
		     exponent--, exp_mask >>= 1) {}
		mantissa = (pcm >> (exponent + 3)) & 0x0F;
	} else {
		exponent = 0;
		mantissa = pcm >> 4;
	}

	return (uint8_t)((sign | (exponent << 4) | mantissa) ^ 0xD5);
}

static uint8_t pcm16_to_ulaw(int16_t pcm_in)
{
	int pcm = pcm_in;
	int sign = 0;
	int exponent = 7;
	int mantissa;

	if (pcm < 0) {
		pcm = -pcm;
		sign = 0x80;
	}
	pcm += 132;
	if (pcm > 32767)
		pcm = 32767;

	for (int exp_mask = 0x4000; !(pcm & exp_mask) && exponent > 0;
	     exponent--, exp_mask >>= 1) {}
	mantissa = (pcm >> (exponent + 3)) & 0x0F;
	return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

size_t audio_codec_encode_g711(const int16_t *pcm, size_t num_samples,
	uint8_t *out, int codec_type)
{
	for (size_t i = 0; i < num_samples; i++) {
		out[i] = (codec_type == AUDIO_CODEC_TYPE_G711A)
			? pcm16_to_alaw(pcm[i]) : pcm16_to_ulaw(pcm[i]);
	}
	return num_samples;
}

int audio_codec_opus_init(AudioCodecOpus *opus, uint32_t sample_rate,
	uint32_t channels)
{
	typedef void *(*fn_create_t)(int32_t, int, int, int *);
	fn_create_t fn_create;
	int err = 0;

	if (!opus)
		return -1;
	memset(opus, 0, sizeof(*opus));

	opus->lib = dlopen("libopus.so", RTLD_NOW | RTLD_GLOBAL);
	if (!opus->lib) {
		fprintf(stderr,
			"[audio] WARNING: libopus.so not available: %s; "
			"falling back to pcm\n", dlerror());
		return -1;
	}
	fn_create = (fn_create_t)(uintptr_t)dlsym(opus->lib,
		"opus_encoder_create");
	if (!fn_create) {
		fprintf(stderr,
			"[audio] WARNING: opus_encoder_create missing; "
			"falling back to pcm\n");
		dlclose(opus->lib);
		opus->lib = NULL;
		return -1;
	}
	opus->encoder = fn_create((int32_t)sample_rate, (int)channels,
		AUDIO_CODEC_OPUS_APPLICATION_AUDIO, &err);
	if (!opus->encoder || err != 0) {
		fprintf(stderr,
			"[audio] WARNING: opus_encoder_create failed (err=%d); "
			"falling back to pcm\n", err);
		dlclose(opus->lib);
		opus->lib = NULL;
		opus->encoder = NULL;
		return -1;
	}
	opus->encode = (int32_t (*)(void *, const int16_t *, int, uint8_t *,
		int32_t))(uintptr_t)dlsym(opus->lib, "opus_encode");
	opus->destroy = (void (*)(void *))(uintptr_t)dlsym(opus->lib,
		"opus_encoder_destroy");
	if (!opus->encode) {
		fprintf(stderr,
			"[audio] WARNING: opus_encode missing; "
			"falling back to pcm\n");
		audio_codec_opus_teardown(opus);
		return -1;
	}
	return 0;
}

void audio_codec_opus_teardown(AudioCodecOpus *opus)
{
	if (!opus)
		return;
	if (opus->encoder && opus->destroy)
		opus->destroy(opus->encoder);
	if (opus->lib)
		dlclose(opus->lib);
	memset(opus, 0, sizeof(*opus));
}

/* ── Stdout filter (singleton, refcounted) ───────────────────────────── */

static struct {
	pthread_t       thread;
	pthread_mutex_t lock;
	int             pipe_read;
	int             real_stdout;
	int             refcount;
	int             active;
} g_filter = { 0, PTHREAD_MUTEX_INITIALIZER, -1, -1, 0, 0 };

static void *stdout_filter_fn(void *arg)
{
	(void)arg;
	int rfd = g_filter.pipe_read;
	int wfd = g_filter.real_stdout;
	char buf[1024];
	char line[1024];
	int lpos = 0;
	ssize_t n;

	while ((n = read(rfd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			char c = buf[i];
			if (lpos < (int)sizeof(line) - 1)
				line[lpos++] = c;
			if (c == '\n' || lpos == (int)sizeof(line) - 1) {
				if (lpos > 0 && (unsigned char)line[0] != 0x1B)
					(void)write(wfd, line, (size_t)lpos);
				lpos = 0;
			}
		}
	}
	if (lpos > 0 && (unsigned char)line[0] != 0x1B)
		(void)write(wfd, line, (size_t)lpos);
	return NULL;
}

void audio_codec_stdout_filter_start(void)
{
	int pipefd[2];

	pthread_mutex_lock(&g_filter.lock);
	if (g_filter.refcount > 0) {
		g_filter.refcount++;
		pthread_mutex_unlock(&g_filter.lock);
		return;
	}
	if (pipe(pipefd) != 0) {
		pthread_mutex_unlock(&g_filter.lock);
		return;
	}
	fflush(stdout);
	g_filter.real_stdout = dup(STDOUT_FILENO);
	g_filter.pipe_read   = pipefd[0];
	dup2(pipefd[1], STDOUT_FILENO);
	close(pipefd[1]);
	if (pthread_create(&g_filter.thread, NULL, stdout_filter_fn, NULL) != 0) {
		dup2(g_filter.real_stdout, STDOUT_FILENO);
		close(g_filter.real_stdout);
		close(pipefd[0]);
		g_filter.real_stdout = -1;
		g_filter.pipe_read   = -1;
		pthread_mutex_unlock(&g_filter.lock);
		return;
	}
	g_filter.active   = 1;
	g_filter.refcount = 1;
	pthread_mutex_unlock(&g_filter.lock);
}

int audio_codec_stdout_filter_real_fd(void)
{
	int fd;
	pthread_mutex_lock(&g_filter.lock);
	fd = g_filter.active ? g_filter.real_stdout : STDOUT_FILENO;
	pthread_mutex_unlock(&g_filter.lock);
	return fd;
}

void audio_codec_stdout_filter_stop(void)
{
	pthread_mutex_lock(&g_filter.lock);
	if (g_filter.refcount == 0 || !g_filter.active) {
		pthread_mutex_unlock(&g_filter.lock);
		return;
	}
	if (--g_filter.refcount > 0) {
		pthread_mutex_unlock(&g_filter.lock);
		return;
	}
	fflush(stdout);
	dup2(g_filter.real_stdout, STDOUT_FILENO);
	pthread_mutex_unlock(&g_filter.lock);

	pthread_join(g_filter.thread, NULL);

	pthread_mutex_lock(&g_filter.lock);
	g_filter.active = 0;
	close(g_filter.pipe_read);
	close(g_filter.real_stdout);
	g_filter.pipe_read   = -1;
	g_filter.real_stdout = -1;
	pthread_mutex_unlock(&g_filter.lock);
}
