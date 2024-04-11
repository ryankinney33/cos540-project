#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

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

#include "protocol.h"

/* Ideally, these would be set by the Makefile... */
#define SRV_TCP_A   "0.0.0.0"
#define SRV_TCP_P   27020

/* These are extraneous... */
#define CLI_UDP_P   25567
#define CLI_UDP_A   "127.0.0.1"
#define CLI_TCP_A   "0.0.0.0"
#define CLI_TCP_P   27021

/* Global constant data */
// static const CompletePacket_t done = CONTROL_HEADER_DEFAULT;

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

int main() {
	/* file descriptors */
	int serv_tcp_fd;
	int client_tcp_fd;
	int client_udp_fd;
	int local_file_fd;

	/* address */
	struct sockaddr_in client_addr;

	uint16_t blocksize = 4096;

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

	FileBlockPacket_t *block = malloc(sizeof(FileBlockPacket_t) + ntohs(f_info.blocksize));
	block->index = 0;
	ssize_t tmp;
	tmp = read(local_file_fd, block->data_stream, ntohs(f_info.blocksize));
	sendto(client_udp_fd, block, sizeof(FileBlockPacket_t) + tmp, 0, (struct sockaddr *)&client_addr, sizeof(client_addr));
	tmp = read(local_file_fd, block->data_stream, ntohs(f_info.blocksize));
	sendto(client_udp_fd, block, sizeof(FileBlockPacket_t) + tmp, 0, (struct sockaddr *)&client_addr, sizeof(client_addr));

	close(client_tcp_fd);
	close(serv_tcp_fd);

	return 0;
}