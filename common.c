#include "common.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

/* Global constants */
const CompletePacket_t DONE = {.header = CTRL_HEADER_INITIALIZER(PTYPE_COMPLETE)};
const UDPReadyPacket_t READY = {.header = CTRL_HEADER_INITIALIZER(PTYPE_UDPRDY)};

uint16_t parse_port(const char *port_str) {
	errno = 0;
	char *ptr;
	unsigned long tmp = strtoul(port_str, &ptr, 0);
	if (errno || *ptr || tmp > UINT16_MAX) { /* Check if legal */
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
		fprintf(stderr, "inet_pton: "ERRPREFIX"invalid IP address\n");
		errno = EINVAL;
	} else if (err == -1) {
		fprintf(stderr, "inet_pton: "ERRPREFIX"%s\n", strerror(errno));
	}

	return address;
}

size_t get_num_missing(const ACKPacket_t *sack, size_t num_blocks) {
	size_t count = 0; /* Counts the number of blocks received */

	/* Counts the number of set bits in the SACK */
	for (size_t idx = 0; idx < ntohl(sack->length) + 1; ++idx) {
		count += __builtin_popcount(sack->ack_stream[idx]);
	}

	/* Total bits - set bits = number of missing blocks */
	return num_blocks - count;
}

int get_socket(struct sockaddr_in *address, int type, bool reuse) {
	int fd;

	/* Open the wanted socket */
	fd = socket(AF_INET, type, 0);
	if (fd == -1) {
		fprintf(stderr, "socket: "ERRPREFIX"%s\n", strerror(errno));
		return -1;
	}

	/* Avoid time-wait state on bound sockets if wanted */
	if (reuse) {
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) == -1) {
			fprintf(stderr, "setsockopt: "WARNPREFIX"%s\n", strerror(errno)); /* Not a fatal error... */
		}
	}

	/* Attempt to bind the socket to the wanted address */
	if (bind(fd, (struct sockaddr*)address, sizeof(*address)) == -1) {
		fprintf(stderr, "bind: "ERRPREFIX"%s\n", strerror(errno));
		return -1;
	}
	return fd;
}
