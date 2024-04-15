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

/* These are extraneous... */
#define CLI_TCP_A   "0.0.0.0"
#define CLI_TCP_P   27021

struct udp_worker_arg {
	FileBlockPacket_t *f_block;
	int file_fd;
	int socket_fd;
	int error_code;
	uint16_t block_packet_len;
};

/* Global data */
static const CompletePacket_t done = CONTROL_HEADER_DEFAULT;
static ACKPacket_t *sack_packet; /* Used to store the blocks received bitmap */
static size_t num_blocks_total; /* Total number of blocks to be received */
static volatile bool file_complete = false; /* All the blocks have been received */
static volatile bool round_over = false; /* All blocks in current round received */
static volatile bool complete_recv = false; /* A "Complete" packet from server received */

/* Locks */
// static pthread_mutex_t all_recv_lock = PTHREAD_MUTEX_INITIALIZER;
// static pthread_mutex_t done_recv_lock = PTHREAD_MUTEX_INITIALIZER;

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

	/* Open the UDP socket */
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
static inline void set_block_status(uint32_t idx) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	/* 1 << (idx % 32) sets the bit at the correct position */
	sack_packet->ack_stream[idx / 32] |=  1 << (idx % 32);
}

static inline bool get_block_status(uint32_t idx) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	return (sack_packet->ack_stream[idx / 32] & (1 << idx % 32) != 0);
}

/* Finds the number of blocks missing */
static size_t get_num_missing(void) {
	size_t count = 0; /* Counts the number of blocks received */

	/* Counts the number of set bits in the SACK */
	for (size_t idx = 0; idx < ntohl(sack_packet->length) + 1; ++idx) {
		count += __builtin_popcount(sack_packet->ack_stream[idx]);
	}

	return num_blocks_total - count;
}

/*
 * Scans the block_status buffer and builds an ACK packet.
 * Returns NULL if all blocks were received or on error.
 * If error, errno will be set to the source of error
 */
static ACKPacket_t *build_ACK_packet(bool isNack) {
	ACKPacket_t *pkt = NULL;
	size_t ack_stream_len;

	/* First, determine if all packets have been found */
	ack_stream_len = get_num_missing();

	if (ack_stream_len) { /* This is nonzero if packets were missed */
		if (isNack) {
			/* Construct the NACK packet to be sent to the server */
			pkt = calloc(1, sizeof(ACKPacket_t) + ack_stream_len * sizeof(uint32_t));
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
			while (pkt_idx < ack_stream_len && sack_idx < num_blocks_total) {
				if (sack_packet->ack_stream[sack_idx / 32] == UINT32_MAX) {
					sack_idx += 32; /* Move to next word*/
				} else {
					if (!get_block_status(sack_idx)) { /* Is the block missing? */
						pkt->ack_stream[pkt_idx++] = htonl(sack_idx);
					}
					sack_idx += 1;
				}
			}
		} else {
			/* SACK packet just returns the entire bitmap */
			return sack_packet;
		}
	}

	return pkt;
}

/************************************************************************************/
/* Worker functions                                                                 */
/************************************************************************************/

/* UDP thread: Read blocks of data from the UDP socket and write them to the file. */
static void *udp_listener_worker(void *arg) {
	ssize_t read_len;

	/* Extract the values from the argument */
	int file_fd = ((struct udp_worker_arg*)arg)->file_fd;
	int socket_fd = ((struct udp_worker_arg*)arg)->socket_fd;
	FileBlockPacket_t *f_block = ((struct udp_worker_arg*)arg)->f_block;
	const uint16_t block_packet_len = ((struct udp_worker_arg*)arg)->block_packet_len;
	const uint16_t block_len = block_packet_len - sizeof(FileBlockPacket_t);

	printf("\nUDP Listener starting:\n");

	/* Read blocks and write them to the file */
	while (!file_complete) {
		// printf("value of round over is %d\n", round_over);
		/* Read blocks until the done recv is received */
		if (!round_over) {
			size_t pkt_recvcount = 0;
			while(!complete_recv) {
				read_len = recvfrom(socket_fd, f_block, block_packet_len, 0, NULL, NULL);
				if (read_len == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						continue;
					} else {
						/* an error occurred, abort */
						perror("recvfrom");
						((struct udp_worker_arg*)arg)->error_code = errno;
						return NULL;
					}
				}

				pkt_recvcount += 1;

				/* Move file to correct position */
				uint32_t idx = ntohl(f_block->index);
				set_block_status(idx);
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
						perror("recvfrom");
						((struct udp_worker_arg*)arg)->error_code = errno;
						return NULL;
					}
				}

				pkt_recvcount += 1;

				/* Move file to correct position */
				uint32_t idx = ntohl(f_block->index);
				set_block_status(idx);
				lseek(file_fd, idx * block_len, SEEK_SET);

				/* Write the block into the file */
				write(file_fd, f_block->data_stream, read_len - sizeof(FileBlockPacket_t));
			}
			round_over = true; /* Mark indicator for round completion */

			/* just assume everything was received */
			printf("Blocks received this round: %zu\n", pkt_recvcount);
		}/* else {
			sched_yield();
		}*/
	}

	printf("file_complete");

	((struct udp_worker_arg*)arg)->error_code = 0;
	return NULL;
}

/* TCP thread: Listen for and transmit control packets */
static void tcp_worker(int sock_fd, bool isNack) {
	ssize_t recv_len;
	CompletePacket_t received_complete;
	ACKPacket_t *ack_pkt = NULL;

	while (!file_complete) {
		/* Receive the COMPLETE message */
		recv_len = recv(sock_fd, &received_complete, sizeof(received_complete), MSG_DONTWAIT); // FIXME: error check
		if (recv_len == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			} else { //TODO: better error checking
				perror("recv");
				file_complete = true;
				return;
			}
		}
		if (received_complete.type == COMPLETE) {
			complete_recv = true;
		}

		/* Wait for the UDP transmissions to stop */
		while (!round_over) {
			sched_yield(); // Allow the UDP thread to work
		}

		/* Build the ACK packet */
		ack_pkt = build_ACK_packet(isNack);
		if (ack_pkt == NULL) { /* Transmission complete if NULL */
			/* Send the COMPLETE message */
			send(sock_fd, &done, sizeof(done), 0);
			file_complete = true;
			return;
		} else {
			/* Transmission continues */
			/* Send the ack packet */
			size_t len = sizeof(ACKPacket_t) + (ntohl(ack_pkt->length) + 1) * sizeof(uint32_t);
			int err = send(sock_fd, ack_pkt, len, 0); /* FIXME: error check */
			if (err == -1) {
				perror("send");
				file_complete = true;
				return;
			}
			if (isNack) { /* Free if using a NACK packet */
				free(ack_pkt);
			}
			round_over = false;

		}
	}
}

/* SACK AND NACK SHOULD BE A COMMAND LINE ARGUMENT */
int main() {
	/* Packets */
	FileInformationPacket_t f_info;
	UDPInformationPacket_t udp_info;

	struct udp_worker_arg udp_arg;

	/* Other */
	int tcp_sock, err;
	pthread_t udp_tid;

	/* Create the file */
	udp_arg.file_fd = creat("out.txt", S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
	if (udp_arg.file_fd == -1) {
		perror("open");
		return errno;
	}

	/* Open the UDP socket and build the UDP info packet */
	udp_arg.socket_fd = get_udp_listener(CLI_UDP_A, CLI_UDP_P); //FIXME: error check these
	udp_info = build_udp_info(udp_arg.socket_fd);

	printf("Socket is bound! Waiting for data on %s:%"PRIu16"\n", CLI_UDP_A, ntohs(udp_info.destination_port));

	tcp_sock = connect_to_server(SRV_TCP_A, SRV_TCP_P);
	if (tcp_sock == -1) {
		return errno;
	}

	/* Receive the file size from the server */
	recv(tcp_sock, &f_info, sizeof(f_info), 0); // FIXME: error check this

	udp_arg.block_packet_len = ntohs(f_info.blocksize) + 1 + sizeof(FileBlockPacket_t);

	udp_arg.f_block = malloc(udp_arg.block_packet_len);
	if (udp_arg.f_block == NULL) {
		/* A fatal error occurred */
		return errno;
	}

	/* Create the SACK bitmap */
	num_blocks_total = ntohl(f_info.num_blocks) + 1;
	uint32_t block_status_word_len = (num_blocks_total % 32) ? (num_blocks_total / 32 + 1) : (num_blocks_total / 32);
	sack_packet = calloc(1, sizeof(ACKPacket_t) + block_status_word_len * sizeof(uint32_t)); /* FIXME: error check this */

	/* Fill in the fields of the SACK packet */
	sack_packet->header = done;
	sack_packet->header.type = SACK;
	sack_packet->length = htonl(block_status_word_len - 1);

	printf("Blocksize: %zu\n", udp_arg.block_packet_len - sizeof(FileBlockPacket_t));
	printf("Number of blocks in file: %zu\n", num_blocks_total);

	/* Spawn the UDP thread */
	err = pthread_create(&udp_tid, NULL, udp_listener_worker, &udp_arg);
	if (err) {
		errno = err;
		perror("pthread_create");
		return errno;
	}

	/* Send the UDP Info to the server */
	send(tcp_sock, &udp_info, sizeof(udp_info), 0);

	/* Begin TCP thread loop */
	tcp_worker(tcp_sock, false);

	pthread_join(udp_tid, NULL);

	close(tcp_sock);
	return 0;
}