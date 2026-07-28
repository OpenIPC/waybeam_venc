/*
 * maruko_scl_ports.c — arbiter for the shared i6c SCL tap (port3).
 *
 * See maruko_scl_ports.h for the port model.  The NPU detector owns the tap
 * for a whole run; a grayscale snapshot wants it for one frame.  Exactly one
 * owner at a time, so a snapshot can never reprogram the port underneath a
 * running detector (which would stomp its geometry mid-inference).
 */

#include "maruko_scl_ports.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_owner[16];   /* "" = free */

int maruko_scl_tap_claim(const char *owner)
{
	int rc = -1;

	if (!owner || !owner[0])
		return -1;
	pthread_mutex_lock(&g_lock);
	if (!g_owner[0]) {
		snprintf(g_owner, sizeof(g_owner), "%s", owner);
		rc = 0;
	} else if (strcmp(g_owner, owner) == 0) {
		rc = 0;   /* idempotent: already ours */
	}
	pthread_mutex_unlock(&g_lock);
	return rc;
}

void maruko_scl_tap_release(const char *owner)
{
	pthread_mutex_lock(&g_lock);
	if (owner && g_owner[0] && strcmp(g_owner, owner) == 0)
		g_owner[0] = '\0';
	pthread_mutex_unlock(&g_lock);
}

void maruko_scl_tap_owner_copy(char *buf, size_t len)
{
	if (!buf || len == 0)
		return;
	pthread_mutex_lock(&g_lock);
	snprintf(buf, len, "%s", g_owner);
	pthread_mutex_unlock(&g_lock);
}

void maruko_scl_tap_reset(void)
{
	pthread_mutex_lock(&g_lock);
	g_owner[0] = '\0';
	pthread_mutex_unlock(&g_lock);
}
