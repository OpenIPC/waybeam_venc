#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DURATION_SEC 10u
#define RECEIVE_BUFFER_BYTES (1024 * 1024)
#define MAX_DATAGRAM_BYTES 65536u

typedef struct {
	uint64_t packets;
	uint64_t bytes;
	uint64_t frames;
	uint64_t sequence_gaps;
	uint64_t payload_type_packets[128];
	uint64_t first_packet_us;
	uint64_t last_packet_us;
	uint64_t frame_first_us;
	uint64_t max_frame_spread_us;
	uint64_t last_marker_us;
	uint64_t max_marker_gap_us;
	uint32_t frame_timestamp;
	uint16_t last_sequence;
	int have_frame;
	int have_sequence;
} ConsumerStats;

static volatile sig_atomic_t g_running = 1;

static uint64_t monotonic_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void handle_signal(int signo)
{
	(void)signo;
	g_running = 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX)
		return -1;
	*value = (uint32_t)parsed;
	return 0;
}

static int read_max_dgram_qlen(void)
{
	FILE *file;
	int value = -1;

	file = fopen("/proc/sys/net/unix/max_dgram_qlen", "r");
	if (!file)
		return -1;
	if (fscanf(file, "%d", &value) != 1)
		value = -1;
	fclose(file);
	return value;
}

static int valid_socket_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!name || !name[0] || strlen(name) >= sizeof(((struct sockaddr_un *)0)->sun_path) - 1)
		return 0;
	for (; *p; ++p) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')
			continue;
		return 0;
	}
	return 1;
}

static uint16_t read_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | p[3];
}

static void observe_rtp(ConsumerStats *stats, const uint8_t *packet,
	size_t packet_size, uint64_t now_us)
{
	uint16_t sequence;
	uint32_t timestamp;

	stats->packets++;
	stats->bytes += packet_size;
	if (stats->first_packet_us == 0)
		stats->first_packet_us = now_us;
	stats->last_packet_us = now_us;

	if (packet_size < 12 || (packet[0] >> 6) != 2)
		return;
	stats->payload_type_packets[packet[1] & 0x7fu]++;

	sequence = read_be16(packet + 2);
	timestamp = read_be32(packet + 4);
	if (stats->have_sequence) {
		uint16_t expected = (uint16_t)(stats->last_sequence + 1u);
		uint16_t gap = (uint16_t)(sequence - expected);

		if (gap < UINT16_MAX / 2u)
			stats->sequence_gaps += gap;
	}
	stats->last_sequence = sequence;
	stats->have_sequence = 1;

	if (!stats->have_frame || timestamp != stats->frame_timestamp) {
		stats->frame_timestamp = timestamp;
		stats->frame_first_us = now_us;
		stats->have_frame = 1;
	}

	if (packet[1] & 0x80u) {
		uint64_t spread_us = now_us - stats->frame_first_us;

		stats->frames++;
		if (spread_us > stats->max_frame_spread_us)
			stats->max_frame_spread_us = spread_us;
		if (stats->last_marker_us != 0) {
			uint64_t marker_gap_us = now_us - stats->last_marker_us;

			if (marker_gap_us > stats->max_marker_gap_us)
				stats->max_marker_gap_us = marker_gap_us;
		}
		stats->last_marker_us = now_us;
		stats->have_frame = 0;
	}
}

static void print_summary(const char *name, const ConsumerStats *stats,
	uint64_t started_us, uint64_t finished_us, uint32_t stall_ms)
{
	uint64_t duration_us = finished_us > started_us ? finished_us - started_us : 1;
	uint64_t bitrate_bps = stats->bytes * 8u * 1000000u / duration_us;

	printf("{\"status\":\"%s\",\"socket\":\"@%s\","
		"\"duration_ms\":%" PRIu64 ",\"packets\":%" PRIu64 ","
		"\"bytes\":%" PRIu64 ",\"bitrate_bps\":%" PRIu64 ","
		"\"frames\":%" PRIu64 ",\"sequence_gaps\":%" PRIu64 ","
		"\"max_frame_spread_us\":%" PRIu64 ","
		"\"max_marker_gap_us\":%" PRIu64 ",\"stall_ms\":%u,"
		"\"payload_type_packets\":{",
		stats->packets > 0 ? "success" : "empty", name,
		duration_us / 1000u, stats->packets, stats->bytes, bitrate_bps,
		stats->frames, stats->sequence_gaps, stats->max_frame_spread_us,
		stats->max_marker_gap_us, stall_ms);
	for (unsigned payload_type = 0, first = 1; payload_type < 128; ++payload_type) {
		if (stats->payload_type_packets[payload_type] == 0)
			continue;
		printf("%s\"%u\":%" PRIu64, first ? "" : ",", payload_type,
			stats->payload_type_packets[payload_type]);
		first = 0;
	}
	printf("}}\n");
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--name NAME] [--duration SEC] "
		"[--stall-after-ms MS --stall-ms MS]\n", argv0);
}

int main(int argc, char **argv)
{
	const char *name = "waybeam_venc_test";
	uint32_t duration_sec = DEFAULT_DURATION_SEC;
	uint32_t stall_after_ms = 0;
	uint32_t stall_ms = 0;
	struct sockaddr_un addr;
	ConsumerStats stats;
	uint8_t *buffer = NULL;
	uint64_t started_us;
	uint64_t deadline_us;
	int stalled = 0;
	int recvbuf = RECEIVE_BUFFER_BYTES;
	int fd = -1;
	int rc = 1;

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
			name = argv[++i];
		} else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
			if (parse_u32(argv[++i], &duration_sec) != 0 || duration_sec == 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (strcmp(argv[i], "--stall-after-ms") == 0 && i + 1 < argc) {
			if (parse_u32(argv[++i], &stall_after_ms) != 0) {
				usage(argv[0]);
				return 2;
			}
		} else if (strcmp(argv[i], "--stall-ms") == 0 && i + 1 < argc) {
			if (parse_u32(argv[++i], &stall_ms) != 0) {
				usage(argv[0]);
				return 2;
			}
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (!valid_socket_name(name) || (stall_ms > 0 && stall_after_ms == 0)) {
		usage(argv[0]);
		return 2;
	}

	buffer = malloc(MAX_DATAGRAM_BYTES);
	if (!buffer) {
		fprintf(stderr, "[unix_dgram_consumer] allocation failed\n");
		return 1;
	}
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "[unix_dgram_consumer] socket failed: %s\n", strerror(errno));
		goto done;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path + 1, name, strlen(name));
	if (bind(fd, (const struct sockaddr *)&addr,
	    (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name))) != 0) {
		fprintf(stderr, "[unix_dgram_consumer] bind @%s failed: %s\n",
			name, strerror(errno));
		goto done;
	}

	(void)signal(SIGINT, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&stats, 0, sizeof(stats));
	started_us = monotonic_us();
	deadline_us = started_us + (uint64_t)duration_sec * 1000000u;
	printf("READY @%s qlen=%d duration_sec=%u\n", name,
		read_max_dgram_qlen(), duration_sec);

	while (g_running) {
		struct pollfd pfd;
		uint64_t now_us = monotonic_us();
		int timeout_ms;
		int polled;

		if (now_us >= deadline_us)
			break;
		if (!stalled && stall_ms > 0 &&
		    now_us - started_us >= (uint64_t)stall_after_ms * 1000u) {
			struct timespec pause;

			printf("STALL_BEGIN after_ms=%u duration_ms=%u\n",
				stall_after_ms, stall_ms);
			pause.tv_sec = stall_ms / 1000u;
			pause.tv_nsec = (long)(stall_ms % 1000u) * 1000000L;
			while (nanosleep(&pause, &pause) != 0 && errno == EINTR && g_running)
				;
			stalled = 1;
			printf("STALL_END\n");
			continue;
		}

		timeout_ms = (int)((deadline_us - now_us) / 1000u);
		if (timeout_ms > 100)
			timeout_ms = 100;
		if (timeout_ms < 1)
			timeout_ms = 1;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		polled = poll(&pfd, 1, timeout_ms);
		if (polled < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[unix_dgram_consumer] poll failed: %s\n",
				strerror(errno));
			goto done;
		}
		if (polled > 0 && (pfd.revents & POLLIN)) {
			ssize_t received = recv(fd, buffer, MAX_DATAGRAM_BYTES, 0);

			if (received > 0)
				observe_rtp(&stats, buffer, (size_t)received, monotonic_us());
			else if (received < 0 && errno != EINTR) {
				fprintf(stderr, "[unix_dgram_consumer] recv failed: %s\n",
					strerror(errno));
				goto done;
			}
		}
	}

	print_summary(name, &stats, started_us, monotonic_us(), stall_ms);
	rc = stats.packets > 0 ? 0 : 3;

done:
	if (fd >= 0)
		close(fd);
	free(buffer);
	return rc;
}
