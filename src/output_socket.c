#include "output_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

static int fill_unix_destination(const char *name,
	struct sockaddr_storage *dst, socklen_t *dst_len)
{
	struct sockaddr_un *addr;
	size_t name_len;

	if (!name || !name[0] || !dst || !dst_len)
		return -1;

	name_len = strlen(name);
	addr = (struct sockaddr_un *)dst;
	if (name_len > sizeof(addr->sun_path) - 2) {
		fprintf(stderr, "[output_socket] unix:// socket name too long\n");
		return -1;
	}

	memset(dst, 0, sizeof(*dst));
	addr->sun_family = AF_UNIX;
	memcpy(addr->sun_path + 1, name, name_len);
	*dst_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
	return 0;
}

static int open_socket(int *socket_handle, VencOutputUriType type)
{
	int domain;

	if (!socket_handle)
		return -1;

	switch (type) {
	case VENC_OUTPUT_URI_UDP:
		domain = AF_INET;
		break;
	case VENC_OUTPUT_URI_UNIX:
		domain = AF_UNIX;
		break;
	default:
		fprintf(stderr, "[output_socket] unsupported socket transport\n");
		return -1;
	}

	*socket_handle = socket(domain, SOCK_DGRAM, 0);
	if (*socket_handle < 0) {
		fprintf(stderr, "[output_socket] socket() failed: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static void close_socket_if_open(int *socket_handle)
{
	if (!socket_handle || *socket_handle < 0)
		return;

	close(*socket_handle);
	*socket_handle = -1;
}

static void disconnect_udp_socket(int socket_handle)
{
	struct sockaddr addr;

	if (socket_handle < 0)
		return;

	memset(&addr, 0, sizeof(addr));
	addr.sa_family = AF_UNSPEC;
	(void)connect(socket_handle, &addr, sizeof(addr));
}

int output_socket_fill_udp_destination(const char *host, uint16_t port,
	struct sockaddr_storage *dst, socklen_t *dst_len)
{
	struct sockaddr_in *addr;

	if (!host || !host[0] || port == 0 || !dst || !dst_len)
		return -1;

	memset(dst, 0, sizeof(*dst));
	addr = (struct sockaddr_in *)dst;
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) {
		fprintf(stderr, "[output_socket] invalid IPv4 address '%s'\n", host);
		return -1;
	}
	*dst_len = sizeof(*addr);
	return 0;
}

int output_socket_fill_destination(const VencOutputUri *uri,
	struct sockaddr_storage *dst, socklen_t *dst_len)
{
	if (!uri || !dst || !dst_len)
		return -1;

	switch (uri->type) {
	case VENC_OUTPUT_URI_UDP:
		return output_socket_fill_udp_destination(uri->host, uri->port,
			dst, dst_len);
	case VENC_OUTPUT_URI_UNIX:
		return fill_unix_destination(uri->endpoint, dst, dst_len);
	default:
		fprintf(stderr, "[output_socket] shm:// is not a datagram socket transport\n");
		return -1;
	}
}

int output_socket_configure(int *socket_handle, struct sockaddr_storage *dst,
	socklen_t *dst_len, VencOutputUriType *transport,
	const VencOutputUri *uri, int requested_connected_udp,
	int *connected_udp)
{
	int want_connected;

	if (!socket_handle || !dst || !dst_len || !transport || !uri)
		return -1;
	if (uri->type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "[output_socket] shm:// requires ring-buffer output\n");
		return -1;
	}

	if (*socket_handle < 0 || *transport != uri->type) {
		close_socket_if_open(socket_handle);
		if (open_socket(socket_handle, uri->type) != 0)
			return -1;
		*transport = uri->type;
	}

	if (output_socket_fill_destination(uri, dst, dst_len) != 0) {
		close_socket_if_open(socket_handle);
		return -1;
	}
	if (!connected_udp)
		return 0;

	want_connected = (uri->type == VENC_OUTPUT_URI_UDP && requested_connected_udp) ?
		1 : 0;
	if (uri->type == VENC_OUTPUT_URI_UDP && !want_connected)
		disconnect_udp_socket(*socket_handle);

	*connected_udp = 0;
	if (!want_connected)
		return 0;

	if (connect(*socket_handle, (const struct sockaddr *)dst, *dst_len) != 0) {
		fprintf(stderr, "[output_socket] UDP connect() failed: %s\n",
			strerror(errno));
		return 0;
	}

	*connected_udp = 1;
	return 0;
}

int output_socket_send_parts(int socket_handle,
	const struct sockaddr_storage *dst, socklen_t dst_len,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	struct iovec vec[3];
	struct msghdr msg;
	int iovcnt;
	ssize_t sent;

	if (socket_handle < 0 || !dst || dst_len == 0 || !header || !payload1 ||
	    header_len == 0 || payload1_len == 0) {
		return -1;
	}

	vec[0].iov_base = (void *)header;
	vec[0].iov_len = header_len;
	vec[1].iov_base = (void *)payload1;
	vec[1].iov_len = payload1_len;
	iovcnt = 2;
	if (payload2 && payload2_len > 0) {
		vec[2].iov_base = (void *)payload2;
		vec[2].iov_len = payload2_len;
		iovcnt = 3;
	}

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *)dst;
	msg.msg_namelen = dst_len;
	msg.msg_iov = vec;
	msg.msg_iovlen = iovcnt;
	sent = sendmsg(socket_handle, &msg, 0);
	return sent < 0 ? -1 : 0;
}
