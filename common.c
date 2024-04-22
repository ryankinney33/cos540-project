#include "common.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>

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
		fprintf(stderr, "inet_pton: invalid IP address\n");
		errno = EINVAL;
	} else if (err == -1) {
		perror("inet_pton");
	}

	return address;
}

/* Sets the flag for block at idx to "received" */
inline void set_block_status(uint32_t idx, ACKPacket_t *sack) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	/* 1 << (idx % 32) sets the bit at the correct position */
	sack->ack_stream[idx / 32] |=  1 << (idx % 32);
}

inline bool get_block_status(uint32_t idx, const ACKPacket_t *sack) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	return ((sack->ack_stream[idx / 32] & (1 << idx % 32)) != 0);
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