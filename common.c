#include "common.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

/* Global constants */
const CompletePacket_t done = CONTROL_HEADER_DEFAULT;
const UDPReadyPacket_t ready = UDP_READY_INITIALIZER;

uint16_t parse_port(const char *port_str) {
	errno = 0;
	char *ptr;
	unsigned long tmp = strtoul(port_str, &ptr, 0);
	if (errno || *ptr || tmp > UINT16_MAX) {
		if (!errno)
			errno = EINVAL;
		return 0;
	}
	return tmp;
}

struct sockaddr_in parse_address(const char *ip_address, uint16_t port) {
	struct sockaddr_in address;
	int err;

	/* Set the IP address */
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	err = inet_pton(AF_INET, ip_address, &address.sin_addr);
	if (err == 0) {
		fprintf(stderr, ERRCOLOR "inet_pton: invalid IP address\n");
		errno = EINVAL;
	} else if (err == -1) {
		perror(ERRCOLOR "inet_pton");
	}

	return address;
}

/* Finds the number of blocks missing */
size_t get_num_missing(const ACKPacket_t *sack, size_t num_blocks) {
	size_t count = 0; /* Counts the number of blocks received */

	/* Counts the number of set bits in the SACK */
	for (size_t idx = 0; idx < ntohl(sack->length) + 1; ++idx) {
		count += __builtin_popcount(sack->ack_stream[idx]);
	}

	return num_blocks - count;
}

/*
 * Opens a socket and binds it to the address.
 * Returns the file descriptor for the socket.
 */
int get_socket(struct sockaddr_in *address, int type, bool reuse) {
	int fd;
	// struct sockaddr_in address;

	fd = socket(AF_INET, type, 0);

	if (fd == -1) {
		fprintf(stderr, ERRCOLOR "socket: %s\x1B[0m\n", strerror(errno));
		return -1;
	}

	/* Avoid time-wait state on bound sockets if wanted */
	if (reuse) {
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) == -1) {
			fprintf(stderr, ERRCOLOR "warning: setsockopt failed: %s\x1B[0m\n", strerror(errno)); /* Not a fatal error... */
		}
	}

	if (bind(fd, (struct sockaddr*)address, sizeof(*address)) == -1) {
		fprintf(stderr, ERRCOLOR "bind: %s\x1B[0m\n", strerror(errno));
		return -1;
	}

	return fd;
}