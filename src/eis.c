#include "eis.h"
#include "eis_gyroglide.h"

#include <stdio.h>
#include <string.h>

EisState *eis_create(const EisConfig *cfg)
{
	if (!cfg)
		return NULL;

	const char *mode = cfg->mode;
	if (!mode || mode[0] == '\0')
		mode = "gyroglide";

	if (strcmp(mode, "gyroglide") == 0)
		return eis_gyroglide_create(cfg);

	fprintf(stderr, "EIS: unknown mode \"%s\", using gyroglide\n", mode);
	return eis_gyroglide_create(cfg);
}
