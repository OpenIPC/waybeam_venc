#ifndef WAYBEAM_QR_FORMAT_H
#define WAYBEAM_QR_FORMAT_H

#include "quirc.h"

/* Validate only the fixed Waybeam QR transport envelope. Payload semantics
 * belong to a later standalone action layer. */
int waybeam_qr_data_valid(const struct quirc_data *data);

#endif /* WAYBEAM_QR_FORMAT_H */
