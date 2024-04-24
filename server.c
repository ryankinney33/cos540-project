#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <getopt.h>

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
#include "common.h"

/* Default IP address. Overrideable via command line arguments. */
#define SRV_TCP_A   "0.0.0.0"
#define SRV_TCP_P   8888

// /* These are not used... */
// #define CLI_UDP_P   27019
// #define CLI_UDP_A   "127.0.0.1"
// #define CLI_TCP_A   "0.0.0.0"
// #define CLI_TCP_P   27021

/* TODO: check ALL header preambles for correctness */

/*******************************************************************************************/
/* Private Utility functions                                                               */
/*******************************************************************************************/

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
		fprintf(stderr, "Error: a blocksize of %"PRIu16" is too small.\n", blocksize);

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

	printf(TCPCOLOR "TCP: The file contains %"PRIu64" blocks of %"PRIu16" bytes.\x1B[0m\n", num_blocks, blocksize);

	FileInformationPacket_t pkt = {.header=CONTROL_HEADER_DEFAULT,
		.num_blocks = htonl(num_blocks - 1),
		.blocksize = htons(blocksize - 1)};
	pkt.header.type = FILEINFO;

	return pkt;
}

/*
 * Gets the next block index to send to client.
 * Returns -1 if there are no more blocks to send
 */
static ssize_t get_next_index(ssize_t previous_index, ACKPacket_t *ack, size_t num_blocks_total) {
	if (ack == NULL) { /* If NULL, round 1 is active, increment the index */
		return previous_index + 1; /* Don't need to check if final block, since that is handled elsewhere in this case */
	}

	if (ack->header.type == SACK) {
		for (size_t i = previous_index + 1; i < num_blocks_total; ++i) {
			if (!get_block_status(i, ack)) { /* TODO: Investigate skipping word if all bits set */
				return i;
			}
		}
	} else { /* NACK */
		for (size_t i = 0; i <= ack->length; ++i) {
			if (ack->ack_stream[i] > previous_index) /* TODO: maybe use a binary search instead of linear? */
				return ack->ack_stream[i];
		}
	}
	return -1; /* Nothing found, return done */
}

/************************************************************************************/
/* Worker functions                                                                 */
/************************************************************************************/

static void *udp_loop(void *arg) {
	/* Extract args */
	struct transmit_state *state = ((struct transmit_state*)arg);

	FileBlockPacket_t *send_buf = state->f_block;
	const size_t num_blocks = state->num_blocks;
	const int file_fd = state->file_fd;
	const int socket_fd = state->udp_socket_fd;
	const uint16_t blocksize = state->block_packet_len - sizeof(FileBlockPacket_t);

	printf(UDPCOLOR "UDP: Ready to send blocks.\x1B[0m\n");

	while(1) {
		uint64_t num_blocks_sent = 0;
		ssize_t num_read, num_sent, block_idx = -1;

		do {
			/* Go to the next block index in the file */
			block_idx = get_next_index(block_idx, state->sack, num_blocks);
			if (block_idx == -1) {
				break; /* Invalid index, we are done */
			}

			/* Go to the correct offset in file */
			if (state->sack != NULL)
				lseek(file_fd, blocksize * block_idx, SEEK_SET);

			/* This automatically handles the size of the final block */
			num_read = read(file_fd, send_buf->data_stream, blocksize);
			if (num_read == -1) {
				/* Error occurred! */
				perror("UDP: read");
				exit(errno);
			} else if (num_read == 0) {
				break; /* Nothing read, nothing to send; we are done */
			}

			/* Set block index */
			send_buf->index = htonl(block_idx);

			/* Send to the client */
			num_sent = send(socket_fd, send_buf, num_read + sizeof(FileBlockPacket_t), 0);
			if (num_sent == -1) {
				perror("UDP: send");
				state->status |= UDP_DONE;
				return NULL;
			}
			num_blocks_sent += 1;
		} while (num_read == blocksize); /* Stop sending if we read the last block */

		printf(UDPCOLOR "UDP: Sent %"PRIu64" blocks to client.\x1B[0m\n", num_blocks_sent);

		/* Signal we are finished and wait for the TCP thread */
		pthread_mutex_lock(&state->lock);
		state->status |= UDP_DONE;
		pthread_cond_signal(&state->udp_done);
		pthread_mutex_unlock(&state->lock);
		sched_yield();

		/* Wait for the TCP thread to finish */
		pthread_mutex_lock(&state->lock);
		while(state->status & UDP_DONE) {
			pthread_cond_wait(&state->udp_done, &state->lock);
		}
		if (state->status & TRANSMISSION_OVER) { /* Check if we are done */
			pthread_mutex_unlock(&state->lock);
			break;
		}
		pthread_mutex_unlock(&state->lock);
	}

	return NULL;
}

void tcp_worker(struct transmit_state *state) {
	ssize_t len;
	ControlHeader_t ctrl;
	uint32_t ack_stream_length = 0;
	size_t ack_pkt_len = 0;

	const int sock_fd = state->tcp_socket_fd;

	while(1) {
		/* Wait for the UDP thread to finish */
		pthread_mutex_lock(&state->lock);
		while(!(state->status & UDP_DONE)) {
			pthread_cond_wait(&state->udp_done, &state->lock);
		}
		free(state->sack); /* We are done with the ack, free it */

		/* Tell the client we are done transmitting blocks */
		len = send(sock_fd, &done, sizeof(done), 0);
		if (len == -1) {
			fprintf(stderr, ERRCOLOR "TCP: send: %s\x1B[0m\n", strerror(errno));
			break;
		}

		/* Receive the control header */
		len = recv(sock_fd, &ctrl, sizeof(ctrl), 0);
		if (len == -1) {
			fprintf(stderr, ERRCOLOR "TCP: recv: %s\x1B[0m\n", strerror(errno));
			exit(errno);
		} else if (len == 0) {
			fprintf(stderr, ERRCOLOR "TCP: error: the client has unexpectedly closed the connection.\x1B[0m\n");
			exit(EXIT_FAILURE);
		}

		if (ctrl.type == COMPLETE) { //TODO: real packet verification
			/* We are done, cleanup */
			printf(TCPCOLOR "TCP: The client has received all blocks. Cleaning up...\x1B[0m\n");
			break;
		} else if (ctrl.type != SACK && ctrl.type != NACK) {
			/* something went wrong */
			printf("TCP: The client sent an unexpected packet. Aborting.\n");
			exit(EXIT_FAILURE);
		}

		printf(TCPCOLOR "TCP: Received a %s mode ACK packet from client. Beginning next round.\x1B[0m\n", ctrl.type == SACK ? "Selective" : "Negative");

		/* Get the length of the ack stream */
		len = recv(sock_fd, &ack_stream_length, sizeof(ack_stream_length), 0);
		if (len == -1) {
			fprintf(stderr, ERRCOLOR "TCP: recv: %s\x1B[0m\n", strerror(errno));
			exit(errno);
		} else if (len == 0) {
			fprintf(stderr, ERRCOLOR "TCP: error: the client has unexpectedly closed the connection.\x1B[0m\n");
			exit(EXIT_FAILURE);
		}

		ack_stream_length = ntohl(ack_stream_length);

		/* Allocate and copy information into the ACK packet */
		ack_pkt_len = (ack_stream_length + 1) * sizeof(uint32_t) + sizeof(ACKPacket_t);
		state->sack = malloc(ack_pkt_len);
		if (state->sack == NULL) {
			fprintf(stderr, ERRCOLOR "TCP: malloc: %s\x1B[0m\n", strerror(errno));
			exit(errno);
		}

		state->sack->header = ctrl;
		state->sack->length = ack_stream_length;

		/* Receive the ACK stream and write it to the ACK packet */
		len = recv(sock_fd, state->sack->ack_stream, ack_pkt_len - sizeof(ACKPacket_t), MSG_WAITALL);
		if (len == -1) {
			fprintf(stderr, ERRCOLOR "TCP: recv: %s\x1B[0m\n", strerror(errno));
			exit(errno);
		} else if (len < (ssize_t)(ack_pkt_len - sizeof(ACKPacket_t))) {
			fprintf(stderr, ERRCOLOR "TCP: error: the client has unexpectedly closed the connection.\x1B[0m\n");
			exit(EXIT_FAILURE);
		}

		/* Convert the packet fields to host order (if NACK) */
		if (state->sack->header.type == NACK) {
			for (size_t i = 0; i <= ack_stream_length; ++i) {
				state->sack->ack_stream[i] = ntohl(state->sack->ack_stream[i]);
			}
		}

		/* Signal to the UDP thread we are starting another round */
		state->status &= ~UDP_DONE;
		pthread_cond_signal(&state->udp_done);
		pthread_mutex_unlock(&state->lock);
	}

	/* We are done, tell UDP to finish up and exit */
	state->status = TRANSMISSION_OVER;
	pthread_cond_signal(&state->udp_done);
	pthread_mutex_unlock(&state->lock);
}

int run_transmission(const char *file_path, struct sockaddr_in *bind_address) {
	/* file descriptors */
	int serv_tcp_fd;

	/* Packets */
	FileInformationPacket_t f_info;

	/* IP address of connected  client */
	struct sockaddr_in client_addr;
	socklen_t client_addr_len = sizeof(client_addr);

	/* Thread state */
	uint16_t blocksize;
	pthread_t udp_tid;
	struct transmit_state state = {.sack=NULL, .lock=PTHREAD_MUTEX_INITIALIZER, .udp_done=PTHREAD_COND_INITIALIZER};

	/* Other */
	char tmp[INET_ADDRSTRLEN];
	ssize_t err_check;

	/* Open the file being transferred and build file info packet */
	state.file_fd = open(file_path, O_RDONLY);
	if (state.file_fd == -1) {
		fprintf(stderr, ERRCOLOR "open: %s\x1B[0m\n", strerror(errno));
		return errno;
	}

	/* Get the TCP listening socket */
	serv_tcp_fd = get_socket(bind_address, SOCK_STREAM, true);
	if (serv_tcp_fd == -1) {
		return errno;
	}
	if (listen(serv_tcp_fd, 1) == -1) {
		perror("listen");
		fprintf(stderr, ERRCOLOR "TCP: listen: %s\x1B[0m\n", strerror(errno));
		return errno;
	}
	printf(TCPCOLOR "TCP: Waiting for a connection on %s:%"PRIu16".\x1B[0m\n",
			inet_ntop(AF_INET, &bind_address->sin_addr, tmp, INET_ADDRSTRLEN),
			ntohs(bind_address->sin_port));

	/* Create the UDP socket */
	state.udp_socket_fd = get_socket(bind_address, SOCK_DGRAM, true);
	if (state.udp_socket_fd == -1)
		return errno;


	/* Receive a connection on the socket */
	state.tcp_socket_fd = accept(serv_tcp_fd, (struct sockaddr*)&client_addr, &client_addr_len);
	if (state.tcp_socket_fd == -1) {
		fprintf(stderr, ERRCOLOR "TCP: accept: %s\x1B[0m\n", strerror(errno));
		return errno;
	}
	printf(TCPCOLOR "TCP: Received a connection from %s.\x1B[0m\n", inet_ntop(AF_INET, &client_addr.sin_addr, tmp, INET_ADDRSTRLEN));

	/* Get the wanted blocksize from the client */
	err_check = recv(state.tcp_socket_fd, &f_info, sizeof(f_info), 0);
	if (err_check == -1) {
		fprintf(stderr, ERRCOLOR "TCP: recv: %s\x1B[0m\n", strerror(errno));
		exit(errno);
	} else if (err_check == 0) {
		fprintf(stderr, ERRCOLOR "TCP: error: the client has unexpectedly closed the connection.\x1B[0m\n");
		exit(EXIT_FAILURE);
	}
	blocksize = ntohs(f_info.blocksize) + 1;

	/* Build the file information packet with the file size and true blocksize */
	f_info = get_fileinfo(state.file_fd, blocksize);

	/* Send the file information packet to the client */
	if (send(state.tcp_socket_fd, &f_info, sizeof(f_info), 0) == -1) {
		fprintf(stderr, ERRCOLOR "TCP: send: %s\x1B[0m\n", strerror(errno));
		return errno;
	}

	/* Receive the UDP information from the client */
	if (recvfrom(state.udp_socket_fd, NULL, 0, 0, (struct sockaddr*)&client_addr, &client_addr_len) == -1) {
		fprintf(stderr, ERRCOLOR "UDP: recvfrom: %s\x1B[0m\n", strerror(errno));
		return errno;
	}
	if (send(state.tcp_socket_fd, &ready, sizeof(ready), 0) == -1) { /* Tell client we received it */
		fprintf(stderr, ERRCOLOR "TCP: send: %s\x1B[0m\n", strerror(errno));
		return errno;
	}

	/* Connect to client UDP socket, so we can use send() instead of sendto(), and store less state information */
	if (connect(state.udp_socket_fd, (struct sockaddr *)&client_addr, sizeof(client_addr)) == -1) {
		fprintf(stderr, ERRCOLOR "UDP: connect: %s\x1B[0m\n", strerror(errno));
		return errno;
	}

	state.block_packet_len = sizeof(FileBlockPacket_t) + ntohs(f_info.blocksize) + 1;
	state.f_block = malloc(state.block_packet_len);
	if (state.f_block == NULL) {
		fprintf(stderr, ERRCOLOR "malloc: %s\x1B[0m\n", strerror(errno));
		return errno;
	}
	state.num_blocks = ntohl(f_info.num_blocks) + 1;

	int err = pthread_create(&udp_tid, NULL, udp_loop, &state);
	if (err == -1) {
		perror("pthread_create");
		return errno;
	}
	tcp_worker(&state);
	pthread_join(udp_tid, NULL);

	close(state.tcp_socket_fd);
	close(state.udp_socket_fd);
	close(serv_tcp_fd);
	close(state.file_fd);

	return 0;
}

int main(int argc, char **argv) {
	const char *file_path = NULL;
	const char *bind_addr = SRV_TCP_A;
	uint16_t bind_port = SRV_TCP_P;
	struct sockaddr_in bind_address;

	/* Flags for command line argument parsing */
	int c;
	bool bflag = false, pflag = false;

	static const char usage[] =
		"Usage %s [OPTION]... FILE\n"
		"Reference server implementation of the PDP-11 for the COS 540 Project.\n"
		"\n"
		"options:\n"
		"-b\t\tbind to this address (default: all interface)\n"
		"-h\t\tshow this help message and exit\n"
		"-p\t\tbind to this port (default %"PRIu16")\n"
		"\n"
		"The file transmission is completed according to RFC COS540.\n";

	/* Process command line arguments */
	while ((c = getopt(argc, argv, "b:hp:")) != -1) {
		switch(c) {
		case 'b':
			if (bflag) {
				fprintf(stderr, "%s: warning: bing address specified multiple times.\n", argv[0]);
			} else {
				bflag = true;
				bind_addr = optarg;
			}
			break;
		case 'h':
			printf(usage, argv[0], SRV_TCP_P);
			return 0;
		case 'p':
			if (pflag) {
				fprintf(stderr, "%s: warning: binding port specified multiple times.\n", argv[0]);
			} else {
				pflag = true;
				bind_port = parse_port(optarg);
				if (errno) {
					fprintf(stderr, "%s: error: invalid port specification: %s\n", argv[0], optarg);
				}
			}
			break;
		case '?':
			fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
			return 1;
		default:
			break;
		}
	}


	/* Pull the file name */
	if (optind == argc) {
		fprintf(stderr, "%s: error, no file specified.\n"
		                "You have to specify the input file name.\n"
				"Try '%s -h' for more information.\n", argv[0], argv[0]);
		return 1;
	}
	file_path = argv[optind];

	/* Process the IP address */
	bind_address = parse_address(bind_addr, bind_port);
	if (errno) {
		fprintf(stderr, ERRCOLOR "TCP: %s is an incorrectly specified address.\x1B[0m\n", bind_addr);
		exit(errno);
	}

	return run_transmission(file_path, &bind_address);
}
