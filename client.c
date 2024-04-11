#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* Sockets */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* File I/O */
#include <fcntl.h>
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

/* Global constant data */
// static const CompletePacket_t done = CONTROL_HEADER_DEFAULT;

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
	fd = socket(AF_INET, SOCK_DGRAM, 0);
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
static UDPInformationPacket_t build_udp_info(int socket_fd) {;
	struct sockaddr_in addr;

	if (getsockname(socket_fd, (struct sockaddr *)&addr, &(socklen_t){sizeof(addr)}) == -1) {
		perror("getsockname");
		exit(errno); /* A fatal error occurred */
	}

	UDPInformationPacket_t pkt = {.header = CONTROL_HEADER_DEFAULT, .destination_port = addr.sin_port};
	return pkt;
}

/* SACK AND NACK SHOULD BE A COMMAND LINE ARGUMENT */
int main() {
	int tcp_sock, udp_sock;

	/* Packets */
	FileInformationPacket_t f_info;
	UDPInformationPacket_t udp_info;

	/* Open the UDP socket and build the UDP info packet */
	udp_sock = get_udp_listener(CLI_UDP_A, CLI_UDP_P); //FIXME: error check these
	udp_info = build_udp_info(udp_sock);

	printf("Socket is bound! Waiting for data on %s:%hu\n", CLI_UDP_A, ntohs(udp_info.destination_port));

	tcp_sock = connect_to_server(SRV_TCP_A, SRV_TCP_P);
	if (tcp_sock == -1) {
		return errno;
	}

	recv(tcp_sock, &f_info, sizeof(f_info), 0);

	uint32_t blocksize = ntohs(f_info.blocksize) + 1;
	uint32_t num_blocks = ntohl(f_info.num_blocks) + 1;

	printf("Blocksize: %hu\n", blocksize);
	printf("Number of blocks in file: %u\n", num_blocks);

	send(tcp_sock, &udp_info, sizeof(udp_info), 0);

	size_t data_block_len = sizeof(FileBlockPacket_t) + blocksize + 1;
	FileBlockPacket_t *data_block = malloc(data_block_len);
	data_block->data_stream[blocksize] = '\0';
	ssize_t tmp;
	tmp = recvfrom(udp_sock, data_block, data_block_len, 0, NULL, NULL);
	data_block->data_stream[tmp - sizeof(FileBlockPacket_t)] = '\0';

	printf("%s", data_block->data_stream);
	tmp = recvfrom(udp_sock, data_block, data_block_len, 0, NULL, NULL);
	data_block->data_stream[tmp - sizeof(FileBlockPacket_t)] = '\0';
	printf("%s", data_block->data_stream);


	close(tcp_sock);
	return 0;
}