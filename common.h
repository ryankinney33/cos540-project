#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#include "packets.h"

/*
 * Extracts and validates the port number stored in a port_str.
 * Returns 0 and updates errno on error.
 */
uint16_t parse_port(const char *port_str);

/*
 * Converts the string ip address and numerical port into a
 * sockaddr_in structure. Updates errno on error.
 */
struct sockaddr_in parse_address(const char *ip_address, uint16_t port);

/* Finds the number of blocks missing */
size_t get_num_missing(const ACKPacket_t *sack, size_t num_blocks);

/*
 * Opens a UDP socket and binds to an address.
 * Returns the file descriptor for the socket.
 */
int get_socket(struct sockaddr_in *address, int type, bool reuse);

/* Sets the bit for block idx in the SACK packet */
static inline void set_block_status(uint32_t idx, ACKPacket_t *sack) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	/* 1 << (idx % 32) sets the bit at the correct position */
	sack->ack_stream[idx / 32] |=  1 << (idx % 32);
}

/* Returns the bit for block idx in the SACK packet */
static inline bool get_block_status(uint32_t idx, const ACKPacket_t *sack) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	return ((sack->ack_stream[idx / 32] & (1 << idx % 32)) != 0);
}

/* Data types used for the threads of the client and server */
typedef enum WorkerStatus {
	UDP_DONE = 1 << 0, /* Set when the UDP thread is done receiving/sending blocks (client/server) */
	TRANSMISSION_OVER = 1 << 1, /* Set when every block was successfully transmitted (client/server) */
	TCP_DONE_RECEIVED = 1 << 2, /* Set when the TCP thread receives a Complete packet from server (client only) */
} WorkerStatus_t;
struct transmit_state {
	pthread_mutex_t lock; /* Avoid race conditions */
	ACKPacket_t *sack; /* Holds the address of the sack packet */
	FileBlockPacket_t *f_block;
	size_t num_blocks;
	int file_fd;
	int tcp_socket_fd;
	int udp_socket_fd;
	int error_code;
	WorkerStatus_t status;
	uint16_t block_packet_len;
};

/* Color codes to spice up the programs a bit */
#define TCPCOLOR "\x1B[1;36m"
#define UDPCOLOR "\x1B[33m"
#define ERRCOLOR "\x1B[1;31m"

/* Global constants */
extern const CompletePacket_t done;
extern const UDPReadyPacket_t ready;

#endif /* COMMON_H */