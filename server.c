#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>

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

/* Default IP address. Will be overrideable by command line arguments. */
#define SRV_TCP_A   "0.0.0.0"
#define SRV_TCP_P   8888

// /* These are not used... */
// #define CLI_UDP_P   27019
// #define CLI_UDP_A   "127.0.0.1"
// #define CLI_TCP_A   "0.0.0.0"
// #define CLI_TCP_P   27021

/* TODO: check ALL header preambles for correctness */

typedef enum WorkerStatus {
	UDP_FINISHED = 1 << 0, /* Set when the UDP thread is done sending blocks */
	CLIENT_DONE = 1 << 1, /* Set when the client has received all blocks */
} WorkerStatus_t;

struct transmit_state {
	pthread_mutex_t lock;
	ACKPacket_t *ack_packet;
	FileBlockPacket_t *f_block;
	size_t num_blocks;
	WorkerStatus_t status;
	int file_fd;
	int socket_fd;
	int error_code;
	uint16_t block_packet_len;
};

/* Global data */
static const CompletePacket_t done = CONTROL_HEADER_DEFAULT;
static const UDPReadyPacket_t ready = UDP_READY_INITIALIZER;

/*
 * Opens a TCP socket.
 * Returns the file descriptor for the socket.
 */
static int get_tcp_listener(const char *ip_addr, uint16_t port) {
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

	printf("Socket is bound! Waiting for a connection on %s:%"PRIu16"\n", ip_addr, port);

	return fd;
}

static int get_udp_socket(const char *ip_addr, uint16_t port) {
	int fd, err;
	struct sockaddr_in address;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == -1) {
		return errno;
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

	printf("Using a blocksize of %"PRIu16".\n", blocksize);
	printf("The total number of blocks is %"PRIu64".\n", num_blocks);

	FileInformationPacket_t pkt = {.header=CONTROL_HEADER_DEFAULT,
		.num_blocks = htonl(num_blocks - 1),
		.blocksize = htons(blocksize - 1)};
	pkt.header.type = FILEINFO;

	return pkt;
}

/* Gets the flag for the block at idx */
static inline bool get_block_status(uint32_t idx, ACKPacket_t *pkt) {
	/* idx / 32 gives word position */
	/* idx % 32 gives bit position */
	return ((pkt->ack_stream[idx / 32] & (1 << idx % 32)) != 0);
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
	int file_fd = state->file_fd;
	int socket_fd = state->socket_fd;
	const uint16_t blocksize = state->block_packet_len - sizeof(FileBlockPacket_t);
	int *error_code = &state->error_code;

	printf("UDP: Starting up...\n");

	/* Acquire the lock */
	pthread_mutex_lock(&state->lock);

	while (!(state->status & CLIENT_DONE)) {
		uint64_t num_blocks_sent = 0;
		ssize_t num_read, num_sent, block_idx = -1;
		do {
			/* Go to the next block index in the file */
			block_idx = get_next_index(block_idx, state->ack_packet, num_blocks);
			if (block_idx == -1) {
				break; /* Invalid index, we are done */
			}

			if (state->ack_packet != NULL)
				lseek(file_fd, blocksize * block_idx, SEEK_SET);

			/* This automatically handles the size of the final block */
			num_read = read(file_fd, send_buf->data_stream, blocksize);
			if (num_read == -1) {
				/* Error occurred! */
				perror("UDP: read");
				state->status |= UDP_FINISHED;
				*error_code = errno;
				return NULL;
			} else if (num_read == 0) {
				break; /* Nothing read, nothing to send */
			}

			/* Set block index */
			send_buf->index = htonl(block_idx);

			/* Send to the client */
			num_sent = send(socket_fd, send_buf, num_read + sizeof(FileBlockPacket_t), 0);
			if (num_sent == -1) {
				perror("UDP: send");
				state->status |= UDP_FINISHED;
				return NULL;
			}
			num_blocks_sent += 1;

		} while (num_read == blocksize);

		printf("UDP: Sent %"PRIu64" blocks to client.\n", num_blocks_sent);

		/* Signal to TCP thread we are done*/
		state->status |= UDP_FINISHED;
		pthread_mutex_unlock(&state->lock);
		sched_yield(); /* Allow TCP thread to execute */

		/* Wait for TCP thread to finish */
		while(1) {
			pthread_mutex_lock(&state->lock); /* Will keep the lock until next transmission is over */
			if (state->status & UDP_FINISHED) {
				pthread_mutex_unlock(&state->lock);
				sched_yield();
			} else {
				break;
			}
		}
	}

	*error_code = 0;
	return NULL;
}

void tcp_worker(int sock_fd, struct transmit_state *state) {
	ssize_t len;
	ControlHeader_t ctrl;
	uint32_t ack_stream_length = 0;
	size_t ack_pkt_len = 0;

	printf("TCP: Starting up...\n");

	while(1) {
		/* Check the state */
		pthread_mutex_lock(&state->lock);
		if (state->status & UDP_FINISHED) { /* Check if UDP thread is finished */
			free(state->ack_packet); /* Done with the ACK, free it */

			/* Send the Complete packet */
			len = send(sock_fd, &done, sizeof(done), 0); /* FIXME: error check this */
			if (len == -1) {
				perror("TCP: send");
				break;
			}

			/* Receive the control header */
			len = recv(sock_fd, &ctrl, sizeof(ctrl), 0); /* FIXME: error check*/

			if (ctrl.type == COMPLETE) {
				/* We are done, cleanup */
				printf("TCP: The client has received all blocks. Cleaning up...\n");
				break;
			} else if (ctrl.type != SACK && ctrl.type != NACK) {
				/* something went wrong */
				printf("TCP: The client sent an unexpected packet. Aborting.\n");
				break;
			}

			printf("TCP: Received a %s mode ACK packet from client. Beginning next round.\n", ctrl.type == SACK ? "Selective" : "Negative");

			/* Get the length of the ack stream */
			len = recv(sock_fd, &ack_stream_length, sizeof(ack_stream_length), 0); /* FIXME: error check */
			ack_stream_length = ntohl(ack_stream_length);

			/* Allocate and copy information into the ACK packet */
			ack_pkt_len = (ack_stream_length + 1) * sizeof(uint32_t) + sizeof(ACKPacket_t);
			state->ack_packet = malloc(ack_pkt_len); /* FIXME: error check this */

			state->ack_packet->header = ctrl;
			state->ack_packet->length = ack_stream_length;

			/* Receive the ACK stream and write it to the ACK packet */
			len = recv(sock_fd, state->ack_packet->ack_stream, ack_pkt_len - sizeof(ACKPacket_t), 0);

			/* Convert the packet fields to host order (if NACK) */
			if (state->ack_packet->header.type == NACK) {
				for (size_t i = 0; i <= ack_stream_length; ++i) {
					state->ack_packet->ack_stream[i] = ntohl(state->ack_packet->ack_stream[i]);
				}
			}

			/* Signal to UDP thread to begin */
			state->status &= ~UDP_FINISHED;
		}
		pthread_mutex_unlock(&state->lock);
		sched_yield(); /* Let the UDP thread execute */
	}

	/* We are done, tell UDP to finish up and exit */
	state->status = CLIENT_DONE;
	pthread_mutex_unlock(&state->lock);
}

int run_transmission(const char *file_path, const char *tcp_listen_addr, uint16_t tcp_listen_port) {
	/* file descriptors */
	int serv_tcp_fd;
	int client_tcp_fd;
	int client_udp_fd;
	int local_file_fd;

	/* Packets */
	FileInformationPacket_t f_info;
	// UDPInformationPacket_t udp_info;
	FileBlockPacket_t *block;

	/* IP address of connected  client */
	struct sockaddr_in client_addr;
	socklen_t client_addr_len = sizeof(client_addr);

	/* Thread state */
	uint16_t blocksize, packet_len;
	pthread_t udp_tid;
	struct transmit_state state = {.ack_packet=NULL, .lock=PTHREAD_MUTEX_INITIALIZER};

	/* Open the file being transferred and build file info packet */
	local_file_fd = open(file_path, O_RDONLY); //FIXME: error check this

	serv_tcp_fd = get_tcp_listener(tcp_listen_addr, tcp_listen_port);
	if (serv_tcp_fd == -1) {
		return errno;
	}

	client_udp_fd = get_udp_socket(tcp_listen_addr, tcp_listen_port);

	/* Receive a connection on the socket */
	client_tcp_fd = accept(serv_tcp_fd, (struct sockaddr*)&client_addr, &client_addr_len);
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

	/* Get the wanted blocksize from the client */
	recv(client_tcp_fd, &f_info, sizeof(f_info), 0); // FIXME: error check this
	blocksize = ntohs(f_info.blocksize) + 1;

	/* Build the file information packet with the file size and true blocksize */
	f_info = get_fileinfo(local_file_fd, blocksize);

	/* Send the file information packet to the client */
	send(client_tcp_fd, &f_info, sizeof(f_info), 0); //FIXME: error check this

	/* Receive the UDP information from the client */
	recvfrom(client_udp_fd, NULL, 0, 0, (struct sockaddr*)&client_addr, &client_addr_len);
	send(client_tcp_fd, &ready, sizeof(ready), 0); //FIXME: error check this
	printf("Client is listening on port %"PRIu16"\n\n", ntohs(client_addr.sin_port));

	/* Connect to client UDP socket, so we can use send() instead of sendto(), and store less state information */
	if (connect(client_udp_fd, (struct sockaddr *)&client_addr, sizeof(client_addr)) == -1) {
		perror("connect");
		return errno; /* TODO: cleanup */
	}

	packet_len = sizeof(FileBlockPacket_t) + ntohs(f_info.blocksize) + 1;
	block = malloc(packet_len);

	state.f_block = block;
	state.num_blocks = ntohl(f_info.num_blocks) + 1;
	state.file_fd = local_file_fd;
	state.socket_fd = client_udp_fd;
	state.error_code = 0;
	state.block_packet_len = packet_len;

	int err = pthread_create(&udp_tid, NULL, udp_loop, &state);
	if (err == -1) {
		perror("pthread_create");
		return errno;
	}
	pthread_detach(udp_tid);

	tcp_worker(client_tcp_fd, &state);

	close(client_tcp_fd);
	close(client_udp_fd);
	close(serv_tcp_fd);
	close(local_file_fd);

	return 0;
}

int main() {


	run_transmission("RFC.txt", SRV_TCP_A, SRV_TCP_P);

	return 0;
}
