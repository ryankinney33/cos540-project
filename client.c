#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>

/* Sockets */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* File I/O */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Threading */
#include <pthread.h>
#include <sched.h>

/* Protocol */
#include "packets.h"

/* Ideally, these would be set by the Makefile... */
#define SRV_TCP_A   "127.0.0.1"
#define SRV_TCP_P   27020
#define CLI_UDP_A   "0.0.0.0"
#define CLI_UDP_P   0

/* These are not used... */
#define CLI_TCP_A   "0.0.0.0"
#define CLI_TCP_P   27021

typedef enum WorkerStatus {
	TCP_DONE_RECEIVE = 1 << 0, /* Set when the TCP thread receives a Complete packet from server */
	UDP_NO_MORE_BLOCKS = 1 << 1, /* Set when the UDP thread is done receiving blocks */
	FILE_IS_COMPLETE = 1 << 2 /* Set when every block has been received from the server */
} WorkerStatus_t;

struct transmit_state {
	pthread_mutex_t lock;
	ACKPacket_t *sack;
	FileBlockPacket_t *f_block;
	size_t num_blocks;
	int file_fd;
	int tcp_socket_fd;
	int udp_socket_fd;
	int error_code;
	WorkerStatus_t status;
	uint16_t block_packet_len;
};

/* Global data */
static const CompletePacket_t done = CONTROL_HEADER_DEFAULT;

/* FIXME: Use locking to fix synchro issues */

/*******************************************************************************************/
/* Utility functions                                                                       */
/*******************************************************************************************/

/* Connects to the server's TCP socket listening at the specified address */
static int connect_to_server(char *ip_addr, uint16_t port) {
	int fd, err;
	struct sockaddr_in address;

	/* Open the TCP socket */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		return -1;
	}

	/* Set the IP address */
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	err = inet_pton(AF_INET, ip_addr, &address.sin_addr);
	if (err == 0) {
		fprintf(stderr, "inet_pton: invalid IP address\n");
		errno = EINVAL;
		return -1;
	} else if (err == -1) {
		perror("inet_pton");
		return -1;
	}

	printf("Connecting to %s:%hu\n", ip_addr, port);
	if (connect(fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
		perror("connect");
		return -1;
	}
	printf("Connection successful.\n");
	return fd;
}

/*
 * Opens a UDP socket to receive data from the server.
 * Returns the file descriptor for the socket.
 */
static int get_udp_listener(char *ip_addr, uint16_t port) {
	int fd, err;
	struct sockaddr_in address;

	/* Open the UDP socket (nonblocking) */
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd == -1) {
		perror("socket");
		return -1;
	}

	/* Set the IP address */
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	err = inet_pton(AF_INET, ip_addr, &address.sin_addr);
	if (err == 0) {
		fprintf(stderr, "inet_pton: invalid IP address\n");
		errno = EINVAL;
		return -1;
	} else if (err == -1) {
		perror("inet_pton");
		return -1;
	}

	if (bind(fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
		perror("bind");
		return -1;
	}

	return fd;
}

/* Builds the UDP Information Packet to be sent to the server */
static UDPInformationPacket_t build_udp_info(int socket_fd) {
	struct sockaddr_in addr;

	if (getsockname(socket_fd, (struct sockaddr *)&addr, &(socklen_t){sizeof(addr)}) == -1) {
		perror("getsockname");
		exit(errno); /* A fatal error occurred */
	}

	UDPInformationPacket_t pkt = {.header = CONTROL_HEADER_DEFAULT, .destination_port = addr.sin_port};
	return pkt;
}

/* Sets the flag for block at idx to "received" */
static inline void set_block_status(uint32_t idx, ACKPacket_t *sack) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	/* 1 << (idx % 32) sets the bit at the correct position */
	sack->ack_stream[idx / 32] |=  1 << (idx % 32);
}

static inline bool get_block_status(uint32_t idx, const ACKPacket_t *sack) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	return ((sack->ack_stream[idx / 32] & (1 << idx % 32)) != 0);
}

/* Finds the number of blocks missing */
static size_t get_num_missing(const ACKPacket_t *sack, size_t num_blocks) {
	size_t count = 0; /* Counts the number of blocks received */

	/* Counts the number of set bits in the SACK */
	for (size_t idx = 0; idx < ntohl(sack->length) + 1; ++idx) {
		count += __builtin_popcount(sack->ack_stream[idx]);
	}

	return num_blocks - count;
}

/*
 * Scans the block_status buffer and builds an ACK packet.
 * Returns NULL if all blocks were received or on error.
 * If error, errno will be set to the source of error
 */
static ACKPacket_t *build_ACK_packet(bool isNack, ACKPacket_t *sack, const size_t num_blocks) {
	ACKPacket_t *pkt = NULL;
	size_t ack_stream_len;

	/* First, determine if all packets have been found */
	ack_stream_len = get_num_missing(sack, num_blocks);

	if (ack_stream_len) { /* This is nonzero if packets were missed */
		if (isNack) {
			/* Construct the NACK packet to be sent to the server */
			pkt = malloc(sizeof(ACKPacket_t) + ack_stream_len * sizeof(uint32_t));
			if (pkt == NULL) {
				errno = ENOMEM;
				return NULL;
			}

			/* Create preamble */
			pkt->header.head[0] = 'P';
			pkt->header.head[1] = 'D';
			pkt->header.head[0] = 'P';
			pkt->header.type = NACK;

			/* Set the length */
			pkt->length = htonl(ack_stream_len - 1);

			/* Iterate through the SACK to fill NACK */
			uint32_t sack_idx = 0, pkt_idx = 0;
			while (pkt_idx < ack_stream_len && sack_idx < num_blocks) {
				if (sack->ack_stream[sack_idx / 32] == UINT32_MAX) {
					sack_idx += 32; /* Move to next word*/
				} else {
					if (!get_block_status(sack_idx, sack)) { /* Is the block missing? */
						pkt->ack_stream[pkt_idx++] = htonl(sack_idx);
					}
					sack_idx += 1;
				}
			}
		} else {
			/* SACK packet just returns the entire bitmap */
			pkt = sack;
		}
	}

	return pkt;
}

/************************************************************************************/
/* Worker functions                                                                 */
/************************************************************************************/

/* UDP thread: Read blocks of data from the UDP socket and write them to the file. */
static void *udp_worker(void *arg) {
	ssize_t read_len;

	struct transmit_state *state = (struct transmit_state*)arg;

	/* Extract the values from the argument */
	const int file_fd = state->file_fd;
	const int socket_fd = state->udp_socket_fd;
	FileBlockPacket_t *f_block = state->f_block;
	const uint16_t block_packet_len = state->block_packet_len;
	const uint16_t block_len = block_packet_len - sizeof(FileBlockPacket_t);

	printf("\nUDP Listener starting:\n");

	/* Read blocks and write them to the file */
	while (!(state->status & FILE_IS_COMPLETE)) {
		// printf("value of round over is %d\n", round_over);
		/* Read blocks until the done recv is received */
		if (!(state->status & UDP_NO_MORE_BLOCKS)) {
			size_t pkt_recvcount = 0;
			while(!(state->status & TCP_DONE_RECEIVE)) {
				read_len = recvfrom(socket_fd, f_block, block_packet_len, 0, NULL, NULL);
				if (read_len == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						continue;
					} else {
						/* an error occurred, abort */
						perror("UDP: recvfrom");
						((struct transmit_state*)arg)->error_code = errno;
						return NULL;
					}
				}

				pkt_recvcount += 1;

				/* Move file to correct position */
				uint32_t idx = ntohl(f_block->index);
				set_block_status(idx, state->sack);
				lseek(file_fd, idx * block_len, SEEK_SET);

				/* Write the block into the file */
				write(file_fd, f_block->data_stream, read_len - sizeof(FileBlockPacket_t));
			}

			/* COMPLETE packet was received, read blocks until the internal buffer is empty */
			while(1) {
				read_len = recvfrom(socket_fd, f_block, block_packet_len, 0, NULL, NULL);
				if (read_len == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						break;
					} else {
						perror("UDP: recvfrom");
						((struct transmit_state*)arg)->error_code = errno;
						return NULL;
					}
				}

				pkt_recvcount += 1;

				/* Move file to correct position */
				uint32_t idx = ntohl(f_block->index);
				set_block_status(idx, state->sack);
				lseek(file_fd, idx * block_len, SEEK_SET);

				/* Write the block into the file */
				write(file_fd, f_block->data_stream, read_len - sizeof(FileBlockPacket_t));
			}
			state->status |= UDP_NO_MORE_BLOCKS; /* Mark indicator for round completion */

			/* just assume everything was received */
			printf("UDP: Blocks received this round: %zu\n", pkt_recvcount);
		} else {
			sched_yield();
		}
	}

	((struct transmit_state*)arg)->error_code = 0;
	return NULL;
}

/* TCP thread: Listen for and transmit control packets */
static void tcp_worker(struct transmit_state *state, bool isNack) {
	ssize_t recv_len;
	CompletePacket_t received_complete;
	ACKPacket_t *ack_pkt = NULL;

	const int sock_fd = state->tcp_socket_fd;


	while (!(state->status & FILE_IS_COMPLETE)) {
		/* Receive the COMPLETE message */
		recv_len = recv(sock_fd, &received_complete, sizeof(received_complete), MSG_DONTWAIT); // FIXME: error check
		if (recv_len == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			} else { //TODO: better error checking
				perror("TCP: recv");
				state->status |= FILE_IS_COMPLETE;
				return;
			}
		}
		if (received_complete.type == COMPLETE) {
			state->status |= TCP_DONE_RECEIVE;
		}

		/* Wait for the UDP transmissions to stop */
		while (!(state->status & UDP_NO_MORE_BLOCKS)) {
			sched_yield(); // Allow the UDP thread to work
		}

		/* Build the ACK packet */
		ack_pkt = build_ACK_packet(isNack, state->sack, state->num_blocks);
		if (ack_pkt == NULL) { /* Transmission complete if NULL */
			/* Send the COMPLETE message */
			send(sock_fd, &done, sizeof(done), 0);
			state->status |= FILE_IS_COMPLETE;
			printf("TCP: All blocks received. Cleaning up...\n");
			return;
		} else {
			/* Transmission continues */
			/* Send the ack packet */
			size_t len = sizeof(ACKPacket_t) + (ntohl(ack_pkt->length) + 1) * sizeof(uint32_t);
			int err = send(sock_fd, ack_pkt, len, 0); /* FIXME: error check */
			if (err == -1) {
				perror("TCP: send");
				state->status |= FILE_IS_COMPLETE;
				return;
			}
			if (isNack) { /* Free if using a NACK packet */
				free(ack_pkt);
			}
			state->status &= ~UDP_NO_MORE_BLOCKS;

		}
	}
}

/* SACK/NACK SHOULD BE A COMMAND LINE ARGUMENT */
int main() {
	/* Packets */
	FileInformationPacket_t f_info;
	UDPInformationPacket_t udp_info;

	/* Other */
	int err;
	pthread_t udp_tid;

	/* State that needs to be maintained for the TCP and UDP workers */
	struct transmit_state state = {.lock = PTHREAD_MUTEX_INITIALIZER};

	/* Create the file */
	state.file_fd = creat("out.txt", S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
	if (state.file_fd == -1) {
		perror("open");
		return errno;
	}

	/* Open the UDP socket and build the UDP info packet */
	state.udp_socket_fd = get_udp_listener(CLI_UDP_A, CLI_UDP_P); //FIXME: error check these
	udp_info = build_udp_info(state.udp_socket_fd);

	printf("Socket is bound! Waiting for data on %s:%"PRIu16"\n", CLI_UDP_A, ntohs(udp_info.destination_port));

	state.tcp_socket_fd = connect_to_server(SRV_TCP_A, SRV_TCP_P);
	if (state.tcp_socket_fd == -1) {
		return errno;
	}

	/* Receive the file size from the server */
	recv(state.tcp_socket_fd, &f_info, sizeof(f_info), 0); // FIXME: error check this

	state.block_packet_len = ntohs(f_info.blocksize) + 1 + sizeof(FileBlockPacket_t);

	state.f_block = malloc(state.block_packet_len);
	if (state.f_block == NULL) {
		/* A fatal error occurred */
		return errno;
	}

	/* Create the SACK bitmap */
	state.num_blocks = ntohl(f_info.num_blocks) + 1;
	uint32_t block_status_word_len = (state.num_blocks % 32) ? (state.num_blocks / 32 + 1) : (state.num_blocks / 32);
	state.sack = calloc(1, sizeof(ACKPacket_t) + block_status_word_len * sizeof(uint32_t)); /* FIXME: error check this */

	/* Fill in the fields of the SACK packet */
	state.sack->header = done;
	state.sack->header.type = SACK;
	state.sack->length = htonl(block_status_word_len - 1);

	printf("Blocksize: %zu\n", state.block_packet_len - sizeof(FileBlockPacket_t));
	printf("Number of blocks in file: %zu\n", state.num_blocks);

	/* Spawn the UDP thread */
	err = pthread_create(&udp_tid, NULL, udp_worker, &state);
	if (err) {
		errno = err;
		perror("pthread_create");
		return errno;
	}

	/* Send the UDP Info to the server */
	send(state.tcp_socket_fd, &udp_info, sizeof(udp_info), 0);

	/* Begin TCP thread loop */
	tcp_worker(&state, false);

	pthread_join(udp_tid, NULL);

	close(state.tcp_socket_fd);
	return 0;
}
