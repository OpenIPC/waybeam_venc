#ifndef OUTPUT_SOCKET_H
#define OUTPUT_SOCKET_H

#include "venc_config.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/** Fill a sockaddr_storage from a parsed udp:// or unix:// destination. */
int output_socket_fill_destination(const VencOutputUri *uri,
	struct sockaddr_storage *dst, socklen_t *dst_len);

/** Fill a UDP sockaddr_storage from host/port values. */
int output_socket_fill_udp_destination(const char *host, uint16_t port,
	struct sockaddr_storage *dst, socklen_t *dst_len);

/** Configure socket + destination for a udp:// or unix:// transport. */
int output_socket_configure(int *socket_handle, struct sockaddr_storage *dst,
	socklen_t *dst_len, VencOutputUriType *transport,
	const VencOutputUri *uri, int requested_connected_udp,
	int *connected_udp);

/** Send one datagram composed of a header and up to two payload fragments.
 *
 *  When @p connected_udp is non-zero the socket is assumed to be connected
 *  (via connect()) and the destination pointer is skipped — the kernel
 *  routes to the connected peer and avoids the per-datagram destination
 *  lookup work. @p dst / @p dst_len may still be passed (they are ignored)
 *  so callers can keep a single parameter list.
 */
int output_socket_send_parts(int socket_handle,
	const struct sockaddr_storage *dst, socklen_t dst_len,
	int connected_udp,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

#endif /* OUTPUT_SOCKET_H */
