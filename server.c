#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
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
#define SRV_TCP_A   "0.0.0.0"
#define SRV_TCP_P   27020

/* These are not used... */
#define CLI_UDP_P   27019
#define CLI_UDP_A   "127.0.0.1"
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
volatile bool round_over = false;
volatile bool transmission_complete = false;

ACKPacket_t *ack_packet = NULL;
pthread_mutex_t ack_packet_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Opens a TCP socket.
 * Returns the file descriptor for the socket.
 */
static int get_tcp_listener(char *ip_addr, uint16_t port) {
	int fd, err;
	struct sockaddr_in address;

	/* Open the TCP socket */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		return -1;
	}

	/* Avoid time-wait */
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) == -1) {
		perror("setsockopt"); /* Not a fatal error... */
	}

	/* Create address to bind to */
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

	/* Bind socket to address */
	if (bind(fd, (struct sockaddr*)&address, sizeof(address)) == -1) {
		perror("bind");
		return -1;
	}

	/* Set socket to listening mode */
	if (listen(fd, 1) == -1) {
		perror("listen");
		return -1;
	}

	printf("Socket is bound! Waiting for a connection on %s:%hu\n", ip_addr, port);

	return fd;
}

/* Gets the file information */
FileInformationPacket_t get_fileinfo(int fd, uint16_t blocksize) {
	struct stat statbuf;
	int err = fstat(fd, &statbuf);
	if (err == -1) {
		perror("stat");
		exit(errno); /* A fatal error occurred... */
	}

	off_t fsize = statbuf.st_size;
	uint64_t num_blocks = fsize / blocksize;
	if (fsize % blocksize) { /* fix if there is remaining data */
		num_blocks += 1;
	}

	/* See if the blocksize is possible */
	if (num_blocks > UINT32_MAX) {
		fprintf(stderr, "Error: a blocksize of %hu is too small.\n", blocksize);

		/* Attempt to get a new blocksize */
		uint64_t minimum_blocksize = fsize / UINT32_MAX;
		if (fsize % UINT32_MAX) { /* fix for remainder */
			minimum_blocksize += 1;
		}
		if (minimum_blocksize > 4096) {
			fprintf(stderr, "Error: the file is too large for this protocol! Exiting.\n");
			exit(EFBIG);
		}
		blocksize = minimum_blocksize;
		num_blocks = fsize / minimum_blocksize;
		if (fsize % blocksize) { /* fix for remainder */
			num_blocks += 1;
		}
	}

	printf("Using a blocksize of %hu.\n", blocksize);
	printf("The total number of blocks is %u.\n", (uint32_t)num_blocks);

	FileInformationPacket_t pkt = {.header=CONTROL_HEADER_DEFAULT,
		.num_blocks = htonl(num_blocks - 1),
		.blocksize = htons(blocksize - 1)};
	pkt.header.type = FILEINFO;

	return pkt;
}

/************************************************************************************/
/* Worker functions                                                                 */
/************************************************************************************/

static void *udp_worker(void *arg) {
	ssize_t num_read, num_sent;
	uint32_t block_idx = 0;
	size_t num_blocks_sent = 0;

	/* Extract args */
	int file_fd = ((struct udp_worker_arg*)arg)->file_fd;
	int socket_fd = ((struct udp_worker_arg*)arg)->socket_fd;
	FileBlockPacket_t *send_buf = ((struct udp_worker_arg*)arg)->f_block;
	const uint16_t blocksize = ((struct udp_worker_arg*)arg)->block_packet_len - sizeof(FileBlockPacket_t);

	printf("\nUDP worker starting up...\n");

	while (!transmission_complete) {
		num_blocks_sent = 0;

		/* Acquire the lock on the ack_packet */
		pthread_mutex_lock(&ack_packet_lock);
		do {
			/* Set to the correct position */
			if (ack_packet != NULL) { /* Only first round has non-NULL ack_packet */
				/* TODO: scan ACK_PACKET to get next idx */
				num_blocks_sent = num_blocks_sent;

				lseek(file_fd, blocksize * block_idx, SEEK_SET);
			} else {
				block_idx += 1;
			}

			/* This automatically handles the size of the final block */
			num_read = read(file_fd, send_buf->data_stream, blocksize);
			if (num_read == -1) {
				/* Error occurred! */
				perror("udp: read");
				round_over = true;
				return NULL;
			} else if (num_read == 0) {
				break; /* Nothing read, nothing to send */
			}

			/* Set block index */
			send_buf->index = htonl(block_idx);

			/* Send to the client */
			num_sent = send(socket_fd, send_buf, num_read + sizeof(FileBlockPacket_t), 0);
			if (num_sent == -1) {
				perror("send");
				round_over = true;
				return NULL;
			}
			num_blocks_sent += 1;

		} while (num_read == blocksize);

		printf("Number of blocks sent this round: %zu\n", num_blocks_sent);

		/* Signal to TCP thread we are done*/
		pthread_mutex_unlock(&ack_packet_lock);
		round_over = true;

	}

	return NULL;
}

void tcp_worker(int sock_fd) {
	ssize_t len;
	ControlHeader_t ctrl;
	uint32_t ack_stream_length = 0;
	size_t ack_pkt_len = 0;

	while(1) {
		/* Wait for the UDP thread to finish */
		while (!round_over) {
			sched_yield();
		}
		pthread_mutex_lock(&ack_packet_lock);

		free(ack_packet); /* Done with the ACK, free it */

		/* Send the Complete packet */
		len = send(sock_fd, &done, sizeof(done), 0); /* FIXME: error check this */
		if (len == -1) {
			perror("tcp: send");
			return;
		}

		/* Receive the control header */
		len = recv(sock_fd, &ctrl, sizeof(ctrl), 0); /* FIXME: error check*/

		if (ctrl.type == COMPLETE) {
			/* We are done, cleanup */
			printf("The client has received all blocks. Cleaning up...\n");
			transmission_complete = true;
			round_over = false; /* Signal to client to move on */
			break;
		} else if (ctrl.type != SACK && ctrl.type != NACK) {
			/* something went wrong */
			printf("The client sent an unexpected header. Aborting.\n");
			transmission_complete = true;
			break;
		}

		/* Get the length of the ack stream */
		len = recv(sock_fd, &ack_stream_length, sizeof(ack_stream_length), 0); /* FIXME: error check */

		/* Allocate and copy information into the ACK packet */
		ack_pkt_len = (ntohl(ack_stream_length) + 1) * sizeof(uint32_t) + sizeof(ACKPacket_t);
		ack_packet = malloc(ack_pkt_len); /* FIXME: error check this */
		ack_packet->header = ctrl;
		ack_packet->length = ack_stream_length;

		/* Receive the ACK stream and write it to the ACK packet */
		len = recv(sock_fd, ack_packet->ack_stream, ack_pkt_len - sizeof(ACKPacket_t), 0);

		/* Signal to UDP thread to begin */
		pthread_mutex_unlock(&ack_packet_lock);
		round_over = false;
	}
}

int main() {
	/* file descriptors */
	int serv_tcp_fd;
	int client_tcp_fd;
	int client_udp_fd;
	int local_file_fd;

	/* address */
	struct sockaddr_in client_addr;

	uint16_t blocksize = 50;

	pthread_t udp_tid;

	/* Packets */
	FileInformationPacket_t f_info;
	UDPInformationPacket_t udp_info;

	/* Open the file being transferred and build file info packet */
	local_file_fd = open("RFC.txt", O_RDONLY); //FIXME: error check this
	f_info = get_fileinfo(local_file_fd, blocksize);

	client_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (client_udp_fd == -1) {
		return errno;
	}

	serv_tcp_fd = get_tcp_listener(SRV_TCP_A, SRV_TCP_P);
	if (serv_tcp_fd == -1) {
		return errno;
	}

	/* Receive a connection on the socket */
	client_tcp_fd = accept(serv_tcp_fd, (struct sockaddr*)&client_addr, &(socklen_t){sizeof(client_addr)});
	if (client_tcp_fd == -1) {
		perror("accept");
		return errno;
	}

	/* Attempt to print a status message */
	{
		char tmp[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &client_addr.sin_addr, tmp, INET_ADDRSTRLEN);
		printf("Received a connection from %s.\n", tmp);
	}

	/* Send the file information packet to the client */
	send(client_tcp_fd, &f_info, sizeof(f_info), 0); //FIXME: error check this

	/* Receive the UDP information packet from the client */
	recv(client_tcp_fd, &udp_info, sizeof(udp_info), 0);

	printf("Client is listening on port %hu\n", ntohs(udp_info.destination_port));

	/* Write the UDP port the client is using into the address structure */
	client_addr.sin_port = udp_info.destination_port;

	/* Connect, so we can use send() instead of sendto() */
	if (connect(client_udp_fd, (struct sockaddr *)&client_addr, sizeof(client_addr)) == -1) {
		perror("connect");
		return errno; /* TODO: cleanup */
	}

	uint16_t packet_len = sizeof(FileBlockPacket_t) + ntohs(f_info.blocksize) + 1;
	FileBlockPacket_t *block = malloc(packet_len);

	struct udp_worker_arg udp_arg = { .f_block=block, .file_fd=local_file_fd, .block_packet_len=packet_len, .socket_fd=client_udp_fd };
	int err = pthread_create(&udp_tid, NULL, udp_worker, &udp_arg);
	if (err == -1) {
		perror("pthread_create");
		return errno;
	}

	tcp_worker(client_tcp_fd);

	// pthread_join(udp_tid, NULL); /* Wait for the UDP thread to exit */

	close(client_tcp_fd);
	close(client_udp_fd);
	close(serv_tcp_fd);
	close(local_file_fd);

	return 0;
}
