#include "test_helpers.h"

#include "maruko_scl_ports.h"

#include <string.h>

/* The i6c SCL tap (port3) is wanted by both the NPU detector, which holds it
 * for a whole run, and the grayscale snapshot, which wants it for one frame.
 * These assert the loser is refused rather than allowed to reprogram it. */
int test_maruko_scl_ports(void)
{
	int failures = 0;
	char owner[16];

	maruko_scl_tap_reset();

	/* Free at rest. */
	maruko_scl_tap_owner_copy(owner, sizeof(owner));
	CHECK("tap free at rest", owner[0] == '\0');

	/* Detector takes the tap for its run. */
	CHECK("detect claim ok", maruko_scl_tap_claim("detect") == 0);
	maruko_scl_tap_owner_copy(owner, sizeof(owner));
	CHECK("detect is owner", strcmp(owner, "detect") == 0);

	/* A snapshot must lose rather than stomp the running detector. */
	CHECK("snapshot refused while detect holds",
		maruko_scl_tap_claim("snapshot") == -1);
	maruko_scl_tap_owner_copy(owner, sizeof(owner));
	CHECK("owner still detect", strcmp(owner, "detect") == 0);

	/* Same-owner claim is idempotent (reload paths re-enter). */
	CHECK("detect reclaim idempotent", maruko_scl_tap_claim("detect") == 0);

	/* A non-owner release must not free someone else's tap. */
	maruko_scl_tap_release("snapshot");
	maruko_scl_tap_owner_copy(owner, sizeof(owner));
	CHECK("release by non-owner no-op", strcmp(owner, "detect") == 0);

	/* Owner release frees it; the snapshot can then take it transiently. */
	maruko_scl_tap_release("detect");
	maruko_scl_tap_owner_copy(owner, sizeof(owner));
	CHECK("tap free after release", owner[0] == '\0');

	CHECK("snapshot claim ok when free",
		maruko_scl_tap_claim("snapshot") == 0);
	CHECK("detect refused while snapshot holds",
		maruko_scl_tap_claim("detect") == -1);
	maruko_scl_tap_release("snapshot");
	maruko_scl_tap_owner_copy(owner, sizeof(owner));
	CHECK("tap free after snapshot", owner[0] == '\0');

	/* Empty/NULL owners are rejected, not treated as a wildcard claim. */
	CHECK("empty owner refused", maruko_scl_tap_claim("") == -1);
	CHECK("null owner refused", maruko_scl_tap_claim(NULL) == -1);

	/* reset() drops a stuck claim (pipeline teardown). */
	CHECK("claim before reset", maruko_scl_tap_claim("detect") == 0);
	maruko_scl_tap_reset();
	maruko_scl_tap_owner_copy(owner, sizeof(owner));
	CHECK("reset frees tap", owner[0] == '\0');

	return failures;
}
