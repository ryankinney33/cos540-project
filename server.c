#include <stdio.h>
#include <errno.h>

/* Sockets */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* File I/O */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>

/* Threading */
#include <pthread.h>

#define SRV_TCP_A   "0.0.0.0"
#define SRV_TCP_P   27020

#define CLI_UDP_A   "127.0.0.1"

/* These are extraneous... */
#define CLI_UDP_P   25567
#define CLI_TCP_A   "0.0.0.0"
#define CLI_TCP_P   27021

/*
 * Opens a TCP socket.
 * Returns the file descriptor for the socket.
 * The address information is written into address.
 */
static int get_tcp_listener(struct sockaddr_in *address) {
	int fd, err;

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
	address->sin_family = AF_INET;
	address->sin_port = htons(SRV_TCP_P);
	err = inet_pton(AF_INET, SRV_TCP_A, &address->sin_addr);
	if (err == 0) {
		fprintf(stderr, "inet_pton: invalid IP address\n");
		errno = EINVAL;
		return -1;
	} else if (err == -1) {
		perror("inet_pton");
		return -1;
	}

	/* Bind socket to address */
	if (bind(fd, (struct sockaddr*)address, sizeof(*address)) == -1) {
		perror("bind");
		return -1;
	}

	/* Set socket to listening mode */
	if (listen(fd, 1) == -1) {
		perror("listen");
		return -1;
	}

	return fd;
}

int main() {
	/* file descriptors */
	int sock_fd;
	int client_fd;

	/* address */
	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;

	/* other */
	char addr_print[16];
	const char *tmp;

	printf("Server starting up...\n");
	sock_fd = get_tcp_listener(&server_addr);
	if (sock_fd == -1) {
		return errno;
	}

	/* Print a status message */
	tmp = inet_ntop(AF_INET, &server_addr.sin_addr, addr_print, sizeof(addr_print));
	if (tmp == NULL) {
		/* Something strange happened... */
		perror("inet_ntop");
	} else {
		printf("Socket is bound! Waiting for a connection on %s:%hu\n", addr_print, SRV_TCP_P);
	}

	/* Receive a connection on the socket */
	client_fd = accept(sock_fd, (struct sockaddr*)&client_addr, &(socklen_t){sizeof(client_addr)});
	if (client_fd == -1) {
		perror("accept");
		return errno;
	}

	/* Receive a buffer from the client */
	int err = recv(client_fd, addr_print, 15, 0);
	addr_print[err] = '\0';

	printf("Received from client: %s\n", addr_print);

	close(client_fd);
	close(sock_fd);

	return 0;
}