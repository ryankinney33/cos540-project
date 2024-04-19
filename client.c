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
#define CLI_UDP_P   0 /* 0 makes the OS automatically choose an available port */

/* These are not used... */
#define CLI_TCP_A   "0.0.0.0"
#define CLI_TCP_P   27021

/* TODO: check ALL header preambles for correctness */

typedef enum WorkerStatus {
	TCP_DONE_RECEIVED = 1 << 0, /* Set when the TCP thread receives a Complete packet from server */
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

/*******************************************************************************************/
/* Utility functions                                                                       */
/*******************************************************************************************/

/* Connects to the server's TCP socket listening at the specified address */
int connect_to_server(const char *ip_addr, uint16_t port) {
	int fd, err;
	struct sockaddr_in address;

	/* Open the TCP socket */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("TCP: socket");
		return -1;
	}

	/* Set the IP address */
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	err = inet_pton(AF_INET, ip_addr, &address.sin_addr);
	if (err == 0) {
		fprintf(stderr, "TCP: inet_pton: invalid IP address\n");
		errno = EINVAL;
		return -1;
	} else if (err == -1) {
		perror("TCP: inet_pton");
		return -1;
	}

	printf("TCP: Connecting to %s:%hu\n", ip_addr, port);
	if (connect(fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
		perror("TCP: connect");
		return -1;
	}
	printf("TCP: Connection successful.\n");
	return fd;
}

/*
 * Opens a UDP socket to receive data from the server.
 * Returns the file descriptor for the socket.
 */
int get_udp_listener(const char *ip_addr, uint16_t port) {
	int fd, err;
	struct sockaddr_in address;

	/* Open the UDP socket (nonblocking) */
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd == -1) {
		perror("UDP: socket");
		return -1;
	}

	// /* Increase the buffer size of the socket */
	// setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){1024 * 1024 * 24}, sizeof(int));

	/* Set the IP address */
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	err = inet_pton(AF_INET, ip_addr, &address.sin_addr);
	if (err == 0) {
		fprintf(stderr, "UDP: inet_pton: invalid IP address\n");
		errno = EINVAL;
		return -1;
	} else if (err == -1) {
		perror("UDP: inet_pton");
		return -1;
	}

	if (bind(fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
		perror("UDP: bind");
		return -1;
	}

	return fd;
}

/* Builds the UDP Information Packet to be sent to the server */
UDPInformationPacket_t build_udp_info(int socket_fd) {
	struct sockaddr_in addr;

	if (getsockname(socket_fd, (struct sockaddr *)&addr, &(socklen_t){sizeof(addr)}) == -1) {
		perror("UDP: getsockname");
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
size_t get_num_missing(const ACKPacket_t *sack, size_t num_blocks) {
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
ACKPacket_t *build_ACK_packet(bool isNack, ACKPacket_t *sack, const size_t num_blocks) {
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
void *udp_loop(void *arg) {
	ssize_t read_len;

	struct transmit_state *state = (struct transmit_state*)arg;

	/* Extract the values from the argument */
	const int file_fd = state->file_fd;
	const int socket_fd = state->udp_socket_fd;
	FileBlockPacket_t *f_block = state->f_block;
	const uint16_t block_packet_len = state->block_packet_len;
	const uint16_t block_len = block_packet_len - sizeof(FileBlockPacket_t);

	printf("UDP: Ready to receive blocks.\n");

	pthread_mutex_lock(&state->lock);
	while (!(state->status & FILE_IS_COMPLETE)) {
		size_t pkt_recvcount = 0;

		/* Read blocks and write them to the file */
		while (!(state->status & UDP_NO_MORE_BLOCKS)) {
			pthread_mutex_unlock(&state->lock);

			read_len = recvfrom(socket_fd, f_block, block_packet_len, 0, NULL, NULL);
			if (read_len == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) { /* No message available yet */
					/* Check if the TCP thread has flagged completion */
					pthread_mutex_lock(&state->lock);
					if (state->status & TCP_DONE_RECEIVED) {
						state->status |= UDP_NO_MORE_BLOCKS; /* Receive no more blocks */
						printf("UDP: Received %zu blocks this round.\n", pkt_recvcount);
					}
					continue;
				}
			}

			pkt_recvcount += 1;

			/* Move file to correct position */
			off_t idx = ntohl(f_block->index);

			lseek(file_fd, idx * block_len, SEEK_SET);

			/* Write the block into the file */
			write(file_fd, f_block->data_stream, read_len - sizeof(*f_block));

			pthread_mutex_lock(&state->lock);
			set_block_status(idx, state->sack); /* Mark the block as acquired */
		}
		pthread_mutex_unlock(&state->lock);
		sched_yield(); /* Allow the TCP thread to work */
		pthread_mutex_lock(&state->lock);
	}

	state->error_code = 0;
	return NULL;
}

/* TCP thread: Listen for and transmit control packets */
void tcp_loop(struct transmit_state *state, bool use_nack) {
	ssize_t recv_len;
	CompletePacket_t received_complete;
	ACKPacket_t *ack_pkt = NULL;

	const int sock_fd = state->tcp_socket_fd;

	while(1) {
		/* Receive the Complete message from the server (nonblocking) */
		recv_len = recv(sock_fd, &received_complete, sizeof(received_complete), MSG_DONTWAIT); /* FIXME: error check */
		if (recv_len == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
		}

		if (received_complete.type != COMPLETE) {
			fprintf(stderr, "Received unexpected packet from server. Aborting.\n");
			exit(EXIT_FAILURE);
		}

		printf("TCP: Received \"Complete\" packet from server.\n");

		/* Update the state */
		pthread_mutex_lock(&state->lock);
		state->status |= TCP_DONE_RECEIVED;

		/* Wait for the UDP listener to stop receiving blocks */
		do {
			pthread_mutex_unlock(&state->lock);
			sched_yield(); /* Allow the UDP thread to work */
			pthread_mutex_lock(&state->lock);
		} while (!(state->status & UDP_NO_MORE_BLOCKS));

		/* Build the ACK packet */
		ack_pkt = build_ACK_packet(use_nack, state->sack, state->num_blocks);
		if (ack_pkt == NULL) { /* Check if the transmission is complete */
			/* Send the complete message to the server */
			send(sock_fd, &done, sizeof(done), 0);
			state->status |= FILE_IS_COMPLETE;
			pthread_mutex_unlock(&state->lock);
			printf("TCP: All blocks received. Cleaning up...\n");
			return;
		} else { /* The transmission continues */
			printf("TCP: Sending a %s mode ACK packet to the server.\n", use_nack ? "Negative" : "Selective");
			size_t len = sizeof(*ack_pkt) + (ntohl(ack_pkt->length) + 1) * sizeof(uint32_t);
			int err = send(sock_fd, ack_pkt, len, 0); /* FIXME: error check */
			if (err == -1) {
				perror("TCP: send");
				state->status |= FILE_IS_COMPLETE;
				return;
			}

			if (use_nack) { /* Prevent leaking memory from NACK mode */
				free(ack_pkt);
			}

			/* Signal to the UDP thread to begin another round */
			state->status &= ~(UDP_NO_MORE_BLOCKS | TCP_DONE_RECEIVED);
			pthread_mutex_unlock(&state->lock);
		}
	}
}

/* Prepares and runs the transmission */ // TODO: should probably change the args to a structure
int run_transmission(const char *file_path, const char *udp_listen_addr, uint16_t udp_port, const char *server_addr, uint16_t server_port, bool use_nack) {
	/* Packets */
	FileInformationPacket_t f_info;
	UDPInformationPacket_t udp_info;

	/* Thread state */
	int err;
	pthread_t udp_tid;
	struct transmit_state state = {.lock = PTHREAD_MUTEX_INITIALIZER};

	/* Create the file */
	state.file_fd = creat(file_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
	if (state.file_fd == -1) {
		perror("open");
		return errno;
	}

	/* Open the UDP socket and build the UDP info packet */
	state.udp_socket_fd = get_udp_listener(udp_listen_addr, udp_port);
	udp_info = build_udp_info(state.udp_socket_fd);
	printf("UDP: Listening on %s:%"PRIu16"\n", CLI_UDP_A, ntohs(udp_info.destination_port));

	/* Connect to the server */
	state.tcp_socket_fd = connect_to_server(server_addr, server_port);
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

	printf("TCP: The file contains %zu blocks of %zu bytes.\n", state.num_blocks, state.block_packet_len - sizeof(FileBlockPacket_t));

	putchar('\n');

	/* Spawn the UDP thread */
	err = pthread_create(&udp_tid, NULL, udp_loop, &state);
	if (err) {
		errno = err;
		perror("pthread_create");
		return errno;
	}

	/* Send the UDP Info to the server */
	send(state.tcp_socket_fd, &udp_info, sizeof(udp_info), 0);

	/* Begin TCP thread loop */
	tcp_loop(&state, use_nack);

	pthread_join(udp_tid, NULL);

	close(state.tcp_socket_fd);
	close(state.udp_socket_fd);
	close(state.file_fd);

	return 0;
}


/* SACK/NACK SHOULD BE A COMMAND LINE ARGUMENT */
int main() {
	/* TODO: analyze cmdline args */

	char *file_path = "out.txt";
	char *udp_listen_addr = CLI_UDP_A;
	char *server_addr = SRV_TCP_A;
	uint16_t udp_port = CLI_UDP_P;
	uint16_t server_port = SRV_TCP_P;
	bool use_nack = false;

	run_transmission(file_path, udp_listen_addr, udp_port, server_addr, server_port, use_nack);

	return 0;
}
