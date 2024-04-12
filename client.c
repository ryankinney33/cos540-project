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
// static const CompletePacket_t done = CONTROL_HEADER_DEFAULT;
static unsigned char *block_status;
static size_t block_status_len;
static volatile bool all_recv = false;
static volatile bool done_recv = false;

/* Locks */
// static pthread_mutex_t all_recv_lock = PTHREAD_MUTEX_INITIALIZER;
// static pthread_mutex_t done_recv_lock = PTHREAD_MUTEX_INITIALIZER;

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
	while (!all_recv) {
		size_t pkt_recvcount = 0;
		/* Read blocks until the done recv is received */
		while(!done_recv) {
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
			block_status[idx] = 1;
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
			block_status[idx] = 1;
			lseek(file_fd, idx * block_len, SEEK_SET);

			/* Write the block into the file */
			write(file_fd, f_block->data_stream, read_len - sizeof(FileBlockPacket_t));
		}
		/* just assume everything was received */
		all_recv = true;
		printf("Blocks received this round: %zu\n", pkt_recvcount);
	}

	((struct udp_worker_arg*)arg)->error_code = 0;
	return NULL;
}

/* SACK AND NACK SHOULD BE A COMMAND LINE ARGUMENT */
int main() {
	/* Packets */
	FileInformationPacket_t f_info;
	UDPInformationPacket_t udp_info;
	CompletePacket_t received_complete;

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

	// FIXME: error check this
	recv(tcp_sock, &f_info, sizeof(f_info), 0);

	udp_arg.block_packet_len = ntohs(f_info.blocksize) + 1 + sizeof(FileBlockPacket_t);

	udp_arg.f_block = malloc(udp_arg.block_packet_len);
	if (udp_arg.f_block == NULL) {
		/* A fatal error occurred */
		return errno;
	}

	/* Create the ack status thing (just bytes for now) */
	block_status_len = ntohl(f_info.num_blocks) + 1;
	block_status = calloc(block_status_len, 1);

	printf("Blocksize: %zu\n", udp_arg.block_packet_len - sizeof(FileBlockPacket_t));
	printf("Number of blocks in file: %"PRIu32"\n", ntohl(f_info.num_blocks) + 1);

	/* Spawn the UDP thread */
	err = pthread_create(&udp_tid, NULL, udp_listener_worker, &udp_arg);
	if (err) {
		errno = err;
		perror("pthread_create");
		return errno;
	}

	/* Send the UDP Info to the server */
	send(tcp_sock, &udp_info, sizeof(udp_info), 0);

	/* Receive the COMPLETE message */
	recv(tcp_sock, &received_complete, sizeof(received_complete), 0);
	if (received_complete.type == COMPLETE) {
		done_recv = true;
	}

	pthread_join(udp_tid, NULL);

	close(tcp_sock);
	return 0;
}