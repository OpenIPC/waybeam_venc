/*
 * star6e_vpe_ports.c — arbiter + observability for the VPE0 scaler outputs.
 *
 * See star6e_vpe_ports.h for the port model.  In short: port1 is a single
 * physical second scaler output that stab framing and the NPU detector both
 * want, so this module hands it to exactly one owner and refuses the second
 * claim; port0 is 1:N-shareable and tracked only so the runtime.vpe_taps block
 * shows who is riding it (main + jpeg + record).
 */

#include "star6e_vpe_ports.h"
#include "venc_api.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_active;             /* between begin() and end() */
static char g_port1_owner[16];    /* "" = free */
static bool g_port0_jpeg;
static bool g_port0_record;

/* Serialize the current tap map and hand it to venc_api for the /api/v1/config
 * runtime block.  Caller holds g_lock.  All labels are static, so the hand-
 * built JSON needs no escaping. */
static void publish_locked(void)
{
	char buf[160];
	char p0[96];
	int n;

	if (!g_active) {
		venc_api_set_vpe_taps(NULL);
		return;
	}

	n = snprintf(p0, sizeof(p0), "\"main\"");
	if (g_port0_jpeg && n < (int)sizeof(p0))
		n += snprintf(p0 + n, sizeof(p0) - n, ",\"jpeg\"");
	if (g_port0_record && n < (int)sizeof(p0))
		n += snprintf(p0 + n, sizeof(p0) - n, ",\"record\"");

	if (g_port1_owner[0])
		snprintf(buf, sizeof(buf),
			"{\"port0\":[%s],\"port1\":\"%s\"}", p0, g_port1_owner);
	else
		snprintf(buf, sizeof(buf),
			"{\"port0\":[%s],\"port1\":null}", p0);
	venc_api_set_vpe_taps(buf);
}

void star6e_vpe_ports_begin(void)
{
	pthread_mutex_lock(&g_lock);
	g_active = true;
	g_port1_owner[0] = '\0';
	g_port0_jpeg = false;
	g_port0_record = false;
	publish_locked();
	pthread_mutex_unlock(&g_lock);
}

void star6e_vpe_ports_end(void)
{
	pthread_mutex_lock(&g_lock);
	g_active = false;
	g_port1_owner[0] = '\0';
	g_port0_jpeg = false;
	g_port0_record = false;
	publish_locked();
	pthread_mutex_unlock(&g_lock);
}

int star6e_vpe_port1_claim(const char *owner)
{
	int rc = -1;

	if (!owner || !owner[0])
		return -1;
	pthread_mutex_lock(&g_lock);
	if (!g_port1_owner[0]) {
		snprintf(g_port1_owner, sizeof(g_port1_owner), "%s", owner);
		publish_locked();
		rc = 0;
	} else if (strcmp(g_port1_owner, owner) == 0) {
		rc = 0;   /* idempotent: already ours */
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}

void star6e_vpe_port1_release(const char *owner)
{
	pthread_mutex_lock(&g_lock);
	if (owner && g_port1_owner[0] &&
	    strcmp(g_port1_owner, owner) == 0) {
		g_port1_owner[0] = '\0';
		publish_locked();
	}
	pthread_mutex_unlock(&g_lock);
}

const char *star6e_vpe_port1_owner(void)
{
	/* port1 is mutated only on the pipeline thread, which is also the sole
	 * caller here (a transient log right after a failed claim), so the
	 * returned pointer is stable for the caller's use. */
	const char *o;

	pthread_mutex_lock(&g_lock);
	o = g_port1_owner[0] ? g_port1_owner : NULL;
	pthread_mutex_unlock(&g_lock);
	return o;
}

void star6e_vpe_port1_owner_copy(char *buf, size_t len)
{
	if (!buf || len == 0)
		return;
	pthread_mutex_lock(&g_lock);
	snprintf(buf, len, "%s", g_port1_owner);
	pthread_mutex_unlock(&g_lock);
}

void star6e_vpe_port0_set(const char *consumer, bool present)
{
	if (!consumer)
		return;
	pthread_mutex_lock(&g_lock);
	if (strcmp(consumer, "jpeg") == 0)
		g_port0_jpeg = present;
	else if (strcmp(consumer, "record") == 0)
		g_port0_record = present;
	publish_locked();
	pthread_mutex_unlock(&g_lock);
}
