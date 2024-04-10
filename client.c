#include <stdio.h>
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

#include "protocol.h"

/* Ideally, these would be set by the Makefile... */
#define SRV_TCP_A   "127.0.0.1"
#define SRV_TCP_P   27020
#define CLI_UDP_A   "0.0.0.0"
#define CLI_UDP_P   25567

/* These are extraneous... */
#define CLI_TCP_A   "0.0.0.0"
#define CLI_TCP_P   27021

static int connect_to_server(char *ip_addr, uint16_t port, struct sockaddr_in *address) {
	int fd, err;

	/* Open the TCP socket */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		return -1;
	}

	/* Set the IP address */
	address->sin_family = AF_INET;
	address->sin_port = htons(port);
	err = inet_pton(AF_INET, ip_addr, &address->sin_addr);
	if (err == 0) {
		fprintf(stderr, "inet_pton: invalid IP address\n");
		errno = EINVAL;
		return -1;
	} else if (err == -1) {
		perror("inet_pton");
		return -1;
	}

	printf("Connecting to %s:%hu\n", ip_addr, port);
	if (connect(fd, (struct sockaddr*)address, sizeof(*address)) == -1) {
		perror("connect");
		return -1;
	}
	printf("Connection successful.\n");
	return fd;
}

/* SACK AND ACK SHOULD BE A COMMAND LINE ARGUMENT */
int main(int argc, char *argv[]) {
	int tcp_sock;
	struct sockaddr_in server_address;

	tcp_sock = connect_to_server(SRV_TCP_A, SRV_TCP_P, &server_address);
	if (tcp_sock == -1) {
		return errno;
	}

	write(tcp_sock, "Hello!", 6);
	close(tcp_sock);
	return 0;
}