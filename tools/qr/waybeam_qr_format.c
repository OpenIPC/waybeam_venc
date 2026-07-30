#include "waybeam_qr_format.h"

#include <stddef.h>

#define WAYBEAM_QR_PAYLOAD_LEN 16

static int waybeam_qr_char_valid(unsigned char c)
{
	static const char alphabet[] =
		"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
	size_t i;

	for (i = 0; alphabet[i]; i++)
		if (c == (unsigned char)alphabet[i])
			return 1;
	return 0;
}

int waybeam_qr_data_valid(const struct quirc_data *data)
{
	int i;

	if (!data || data->version != 1 ||
	    data->ecc_level != QUIRC_ECC_LEVEL_Q ||
	    data->data_type != QUIRC_DATA_TYPE_ALPHA ||
	    data->payload_len != WAYBEAM_QR_PAYLOAD_LEN)
		return 0;
	if (data->payload[0] != 'P' && data->payload[0] != 'C')
		return 0;

	for (i = 0; i < data->payload_len; i++)
		if (!waybeam_qr_char_valid(data->payload[i]))
			return 0;
	return 1;
}
