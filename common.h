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

/* Verifies that the header preamble is correct */
static inline bool verify_header_preamble(const ControlHeader_t *hdr) {
	return hdr->head[0] == 'P' && hdr->head[1] == 'D' && hdr->head[2] == 'P';
}

/* Verifies the header preamble is correct and the packet type matches the expected type */
static inline bool verify_header(const ControlHeader_t *hdr, PType_t expected_type) {
	return verify_header_preamble(hdr) && hdr->type == expected_type;
}

/* Data types used for the threads of the client and server */
typedef enum WorkerStatus {
	UDP_DONE = 1 << 0, /* Set when the UDP thread is done receiving/sending blocks (client/server) */
	TRANSMISSION_OVER = 1 << 1, /* Set when every block was successfully transmitted (client/server) */
	TCP_DONE_RECEIVED = 1 << 2, /* Set when the TCP thread receives a Complete packet from server (client only) */
} WorkerStatus_t;
struct transmit_state {
	pthread_mutex_t lock; /* Avoid race conditions */
	pthread_cond_t udp_done; /* Avoid race conditions */
	ACKPacket_t *sack; /* Holds the address of the sack packet */
	FileBlockPacket_t *f_block; /* The file block packet the UDP thread sends/receives */
	size_t num_blocks; /* The number of blocks in the file */
	int file_fd; /* File descriptor for the file being read from/written to */
	int tcp_socket_fd; /* Socket descriptor for the TCP connection between server and client */
	int udp_socket_fd; /* Socket descriptor for the UDP socket used for sending/receiving bloks */
	WorkerStatus_t status; /* Status flag for the state of the file transmission (no more blocks/transmission is complete, etc.) */
	uint16_t block_packet_len; /* The length of a file block packet*/
};

/* Colored prefixes to spice up the programs a bit */
#define TCPPREFIX  "\x1B[1;36mTCP\x1B[0m: "
#define UDPPREFIX  "\x1B[1;33mUDP\x1B[0m: "
#define ERRPREFIX  "\x1B[1;31merror\x1B[0m: "
#define WARNPREFIX "\x1B[1;38;5;5mwarning\x1B[0m: "

/* Global constants */
extern const CompletePacket_t DONE;
extern const UDPReadyPacket_t READY;

#endif /* COMMON_H */
