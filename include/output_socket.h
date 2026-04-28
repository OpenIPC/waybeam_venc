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

/** Producer-side queue-fill snapshot for udp:// / unix:// outputs.
 *
 * Reads SIOCOUTQ (current bytes in send queue) and divides by
 * @p sndbuf_capacity (kernel-reported SO_SNDBUF, captured once at socket
 * open via output_socket_capture_capacity()).  Pass <= 0 to have the
 * function read SO_SNDBUF live via getsockopt — useful for cold paths
 * and tests; the hot path should always pass the cached value to keep
 * this to a single SIOCOUTQ syscall per call.
 *
 * Linux reports SO_SNDBUF as 2× the requested size (the doubling is
 * internal kernel bookkeeping); both queued and sndbuf use the same
 * units, so the ratio is correct without correcting the doubling.
 *
 * Returns 0 on success and writes 0..100 into *out_pct.  Returns -1
 * for fd < 0 or any of the syscalls failing.  On UDP the queue drains
 * fast (kernel hands to NIC) so values >0 are rare in steady state;
 * on UNIX datagram a slow consumer can hold it pinned near 100. */
int output_socket_get_fill_pct(int socket_handle, int sndbuf_capacity,
	uint8_t *out_pct);

/** Read the kernel-applied SO_SNDBUF for @p socket_handle.  Call once
 * after socket open / reconfigure and cache the result; the kernel
 * doesn't change SO_SNDBUF unless someone calls setsockopt again, so
 * the cached value is stable for the socket's lifetime.
 *
 * Returns 0 on success (writes capacity into *out_capacity), -1 on
 * getsockopt failure or fd < 0. */
int output_socket_capture_capacity(int socket_handle, int *out_capacity);

#endif /* OUTPUT_SOCKET_H */
