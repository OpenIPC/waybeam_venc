#include "test_helpers.h"

#include "star6e_vpe_ports.h"
#include "venc_api.h"

#include <string.h>

/* The arbiter publishes the tap map through venc_api_set_vpe_taps(); read it
 * back through the public getter to assert what /api/v1/config would emit. */
static const char *taps(char *buf, size_t n)
{
	return venc_api_get_vpe_taps(buf, n) ? buf : "(none)";
}

int test_star6e_vpe_ports(void)
{
	int failures = 0;
	char buf[192];

	/* Before begin(): nothing published. */
	star6e_vpe_ports_end();
	CHECK("idle no taps", venc_api_get_vpe_taps(buf, sizeof(buf)) == 0);

	/* begin(): port0 has main, port1 free. */
	star6e_vpe_ports_begin();
	CHECK("begin main only",
		strcmp(taps(buf, sizeof(buf)),
			"{\"port0\":[\"main\"],\"port1\":null}") == 0);

	/* Detector claims port1. */
	CHECK("detect claim ok", star6e_vpe_port1_claim("detect") == 0);
	CHECK("detect owner", star6e_vpe_port1_owner() != NULL &&
		strcmp(star6e_vpe_port1_owner(), "detect") == 0);
	CHECK("detect published",
		strcmp(taps(buf, sizeof(buf)),
			"{\"port0\":[\"main\"],\"port1\":\"detect\"}") == 0);

	/* Second claim by a different owner is refused; owner unchanged. */
	CHECK("stab claim refused", star6e_vpe_port1_claim("stab") == -1);
	CHECK("owner still detect",
		strcmp(star6e_vpe_port1_owner(), "detect") == 0);

	/* Same-owner claim is idempotent. */
	CHECK("detect reclaim idempotent", star6e_vpe_port1_claim("detect") == 0);

	/* A non-owner release is a no-op. */
	star6e_vpe_port1_release("stab");
	CHECK("release by non-owner no-op",
		star6e_vpe_port1_owner() != NULL &&
		strcmp(star6e_vpe_port1_owner(), "detect") == 0);

	/* owner_copy(): private copy for off-pipeline-thread callers (the
	 * snapshot endpoint reports the winner when its claim loses). */
	{
		char owner[16];
		memset(owner, 'x', sizeof(owner));
		star6e_vpe_port1_owner_copy(owner, sizeof(owner));
		CHECK("owner_copy held", strcmp(owner, "detect") == 0);
	}

	/* Owner release frees port1; now stab can take it. */
	star6e_vpe_port1_release("detect");
	CHECK("port1 free after release", star6e_vpe_port1_owner() == NULL);

	/* owner_copy() on a free port yields an empty string, not stale text. */
	{
		char owner[16];
		memset(owner, 'x', sizeof(owner));
		star6e_vpe_port1_owner_copy(owner, sizeof(owner));
		CHECK("owner_copy free is empty", owner[0] == '\0');
	}

	/* A snapshot capture claims port1 transiently, like any other owner. */
	CHECK("snapshot claim ok", star6e_vpe_port1_claim("snapshot") == 0);
	CHECK("stab refused while snapshot holds",
		star6e_vpe_port1_claim("stab") == -1);
	star6e_vpe_port1_release("snapshot");
	CHECK("port1 free after snapshot", star6e_vpe_port1_owner() == NULL);

	CHECK("stab claim ok after free", star6e_vpe_port1_claim("stab") == 0);

	/* port0 consumers accumulate in the published map. */
	star6e_vpe_port0_set("jpeg", true);
	star6e_vpe_port0_set("record", true);
	CHECK("port0 consumers + port1 stab",
		strcmp(taps(buf, sizeof(buf)),
			"{\"port0\":[\"main\",\"jpeg\",\"record\"],"
			"\"port1\":\"stab\"}") == 0);

	star6e_vpe_port0_set("jpeg", false);
	CHECK("port0 jpeg cleared",
		strcmp(taps(buf, sizeof(buf)),
			"{\"port0\":[\"main\",\"record\"],\"port1\":\"stab\"}") == 0);

	/* end(): map hidden again. */
	star6e_vpe_ports_end();
	CHECK("end hides taps", venc_api_get_vpe_taps(buf, sizeof(buf)) == 0);

	return failures;
}
