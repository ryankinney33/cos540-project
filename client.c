#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include <getopt.h>

/* Sockets */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* I/O */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <unistd.h>

/* Threading */
#include <pthread.h>
#include <sched.h>

/* Protocol */
#include "packets.h"
#include "common.h"

/* Defaults. Overridden command line arguments. */
#define SRV_TCP_A   "127.0.0.1"
#define SRV_TCP_P   8888
#define CLI_UDP_A   "0.0.0.0"
#define CLI_UDP_P   0 /* 0 makes the OS automatically choose an available port */

// /* These are not used... */
// #define CLI_TCP_A   "0.0.0.0"
// #define CLI_TCP_P   27021

/* TODO: check ALL header preambles for correctness */

/*******************************************************************************************/
/* Private Utility functions                                                               */
/*******************************************************************************************/

/* Connects to the server's TCP socket listening at the specified address */
static int connect_to_server(struct sockaddr_in *address) {
	int fd;

	/* Open the TCP socket */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		fprintf(stderr, ERRCOLOR "TCP: socket: %s\x1B[0m\n", strerror(errno));
		return -1;
	}

	printf(TCPCOLOR "TCP: Connecting to %s:%"PRIu16"...\x1B[0m\n", inet_ntoa(address->sin_addr), ntohs(address->sin_port)); // TODO: change this
	if (connect(fd, (struct sockaddr *)address, sizeof(*address)) == -1) {
		fprintf(stderr, ERRCOLOR "TCP: connect: %s\x1B[0m\n", strerror(errno));
		return -1;
	}

	printf(TCPCOLOR "TCP: Connection successful.\x1B[0m\n");
	return fd;
}

/*
 * Scans the block_status buffer and builds an ACK packet.
 * Returns NULL if all blocks were received or on error.
 * If error, errno will be set to the source of error
 */
static ACKPacket_t *build_ACK_packet(bool isNack, ACKPacket_t *sack, const size_t num_blocks) {
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
					sack_idx += 32; /* Move to next word */
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
	int8_t successive_zeros = 0; /* Counts up when a round with 0 blocks occurs */

	struct transmit_state *state = (struct transmit_state*)arg;

	const uint64_t interval = state->num_blocks / 5;

	/* Extract the values from the argument */
	const int file_fd = state->file_fd;
	const int socket_fd = state->udp_socket_fd;
	FileBlockPacket_t *f_block = state->f_block;
	const uint16_t block_packet_len = state->block_packet_len;
	const uint16_t block_len = block_packet_len - sizeof(FileBlockPacket_t);

	/* Set up the epoll structure */
	struct epoll_event event, events[1];
	int timeout = 50; /* 50 ms timeout */
	int event_count;
	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		fprintf(stderr, ERRCOLOR "UDP: epoll_create1: %s\x1B[0m\n", strerror(errno));
		exit(errno);
	}
	event.events = EPOLLIN | EPOLLET;
	event.data.fd = socket_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) == -1) {
		fprintf(stderr, ERRCOLOR "UDP: epoll_ctl: %s\x1B[0m\n", strerror(errno));
		exit(errno);
	}

	printf(UDPCOLOR "UDP: Ready to receive blocks.\x1B[0m\n");

	pthread_mutex_lock(&state->lock);
	while (!(state->status & TRANSMISSION_OVER)) {
		uint64_t pkt_recvcount = 0;
		bool round_occurred = false;

		/* Read blocks and write them to the file */
		while (!(state->status & UDP_DONE)) {
			pthread_mutex_unlock(&state->lock);
			round_occurred = true;

			event_count = epoll_wait(epoll_fd, events, 1, timeout);
			if (event_count == -1) {
					fprintf(stderr, ERRCOLOR "UDP: epoll_wait: %s\x1B[0m\n", strerror(errno));
					exit(errno);
			} else if (event_count == 0) { /* Timeout */
				/* Check if the TCP thread has flagged completion */
				pthread_mutex_lock(&state->lock);
				if (state->status & TCP_DONE_RECEIVED) {
					state->status |= UDP_DONE; /* Receive no more blocks */
					printf(UDPCOLOR "UDP: Received %"PRIu64" blocks this round.\x1B[0m\n", pkt_recvcount);
				}
				continue;
			}

			/* The socket is ready */
			while(1) {
				read_len = recvfrom(events[0].data.fd, f_block, block_packet_len, 0, NULL, NULL); /* Read blocks until we get EWOULDBLOCK or EAGAIN */
				if (read_len == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						break;
					}
					fprintf(stderr, ERRCOLOR "UDP: recfrom: %s\x1B[0m\n", strerror(errno));
					exit(1);
				}
				pkt_recvcount += 1;

				/* Extract index from the block and write to file */
				off_t idx = ntohl(f_block->index);
				if (!get_block_status(idx, state->sack)) { /* Only change file if this is a new block */
					/* Set file position and write the block to the file */
					lseek(file_fd, idx * block_len, SEEK_SET);
					write(file_fd, f_block->data_stream, read_len - sizeof(*f_block));
					if (interval && (pkt_recvcount % interval == 0)) {
						printf(UDPCOLOR "UDP: Received %"PRIu64" packets so far this round.\x1B[0m\n", pkt_recvcount);
					}
					set_block_status(idx, state->sack); /* Mark the block as acquired */
				}
			}

			pthread_mutex_lock(&state->lock);
		}

		if (round_occurred && pkt_recvcount == 0) {
			successive_zeros += 1;

			if (successive_zeros >= 2) {
				fprintf(stderr, ERRCOLOR "UDP: 2 rounds in a row with all blocks dropped. Giving up.\x1B[0m\nConsider reducing the blocksize and trying again.\n");
				exit(EXIT_FAILURE);
			}
		} else if (round_occurred) {
			successive_zeros = 0;
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
		recv_len = recv(sock_fd, &received_complete, sizeof(received_complete), MSG_WAITALL); /* FIXME: error check */
		if (recv_len == -1) {
			exit(EXIT_FAILURE);
		}

		if (received_complete.type != COMPLETE) {
			fprintf(stderr, "Received unexpected packet from server. Aborting.\n");
			exit(EXIT_FAILURE);
		}

		printf(TCPCOLOR "TCP: Received \"Complete\" packet from server.\x1B[0m\x1B[0K\n");

		/* Update the state */
		pthread_mutex_lock(&state->lock);
		state->status |= TCP_DONE_RECEIVED;

		/* Wait for the UDP listener to stop receiving blocks */
		do {
			pthread_mutex_unlock(&state->lock);
			sched_yield(); /* Allow the UDP thread to work */
			pthread_mutex_lock(&state->lock);
		} while (!(state->status & UDP_DONE));

		/* Build the ACK packet */
		ack_pkt = build_ACK_packet(use_nack, state->sack, state->num_blocks);
		if (ack_pkt == NULL) { /* Check if the transmission is complete */
			/* Send the complete message to the server */
			send(sock_fd, &done, sizeof(done), 0);
			state->status |= TRANSMISSION_OVER;
			pthread_mutex_unlock(&state->lock);
			printf(TCPCOLOR "TCP: All blocks received. Cleaning up...\x1B[0m\n");
			return;
		} else { /* The transmission continues */
			printf(TCPCOLOR "TCP: Sending a %s mode ACK packet to the server.\x1B[0m\n", use_nack ? "Negative" : "Selective");
			size_t len = sizeof(*ack_pkt) + (ntohl(ack_pkt->length) + 1) * sizeof(uint32_t);
			int err = send(sock_fd, ack_pkt, len, 0); /* FIXME: error check */
			if (err == -1) {
				fprintf(stderr, ERRCOLOR "TCP: send: %s\x1B[0m\n", strerror(errno));
				state->status |= TRANSMISSION_OVER;
				return;
			}

			if (use_nack) { /* Prevent leaking memory from NACK mode */
				free(ack_pkt);
			}

			/* Signal to the UDP thread to begin another round */
			state->status &= ~(UDP_DONE | TCP_DONE_RECEIVED);
			pthread_mutex_unlock(&state->lock);
		}
	}
}

/* Prepares and runs the transmission */
int run_transmission(const char *file_path, struct sockaddr_in *bind_address, struct sockaddr_in *server_address, bool use_nack, uint16_t expected_blocksize) {
	/* Packets */
	FileInformationPacket_t f_info;

	/* Thread state */
	int err;
	pthread_t udp_tid;
	struct transmit_state state = {.lock = PTHREAD_MUTEX_INITIALIZER};

	printf("Using %s ACK mode.\n", (use_nack)? "Negative" : "Selective");

	/* Create the file */
	state.file_fd = creat(file_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
	if (state.file_fd == -1) {
		perror("open");
		return errno;
	}

	/* Open a non-blocking UDP socket and bind it to the wanted address */
	state.udp_socket_fd = get_socket(bind_address, SOCK_DGRAM | SOCK_NONBLOCK, false);

	/* Connect to the server */
	state.tcp_socket_fd = connect_to_server(server_address);
	if (state.tcp_socket_fd == -1) {
		return errno;
	}

	/* Send the wanted blocksize to the server */
	f_info.header.head[0] = 'P';
	f_info.header.head[1] = 'D';
	f_info.header.head[2] = 'P';
	f_info.header.type = FILEINFO;
	f_info.num_blocks = 0;
	f_info.blocksize = htons(expected_blocksize - 1);
	send(state.tcp_socket_fd, &f_info, sizeof(f_info), 0); // FIXME: error check this

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

	printf(TCPCOLOR "TCP: The file contains %zu blocks of %zu bytes.\n\x1B[0m", state.num_blocks, state.block_packet_len - sizeof(FileBlockPacket_t));

	/* Initialize UDP connection with server */
	while(1) {
		err = sendto(state.udp_socket_fd, NULL, 0, 0, (struct sockaddr *)server_address, sizeof(*server_address));
		if (err == -1) {
			fprintf(stderr, ERRCOLOR "UDP: sendto: %s\x1B[0m\n", strerror(errno));
			return errno;
		}

		/* Set 500 ms timeout */
		struct timeval timeout;
		fd_set fdlist;

		timeout.tv_sec = 0;
		timeout.tv_usec = 500000;

		FD_ZERO(&fdlist);
		FD_SET(state.tcp_socket_fd, &fdlist);

		err = select(state.tcp_socket_fd + 1, &fdlist, NULL, NULL, &timeout); /* FIXME: ERROR CHECK */
		if (err == -1) {
			fprintf(stderr, "\x1B[1;TCP: select: %s\x1B[0m\n", strerror(errno));
			return errno;
		}

		/* see if data came in */
		if (FD_ISSET(state.tcp_socket_fd, &fdlist)) {
			/* Read the data */
			UDPReadyPacket_t rdy;
			err = recv(state.tcp_socket_fd, &rdy, sizeof(rdy), 0); /* FIXME: ERROR CHECK */
			if (rdy.type == UDPRDY)
				break;
			break; // TODO: handle this correctly
		}
	}
	putchar('\n');

	/* Spawn the UDP thread */
	err = pthread_create(&udp_tid, NULL, udp_loop, &state);
	if (err) {
		errno = err;
		perror("pthread_create");
		return errno;
	}

	/* Begin TCP thread loop */
	tcp_loop(&state, use_nack);

	pthread_join(udp_tid, NULL);

	close(state.tcp_socket_fd);
	close(state.udp_socket_fd);
	close(state.file_fd);

	return 0;
}

/* Process command line arguments and begin transmission */
int main(int argc, char **argv) {
	const char *file_path = NULL;
	const char *bind_addr = CLI_UDP_A;
	const char *server_addr = SRV_TCP_A;
	uint16_t bind_port = CLI_UDP_P;
	uint16_t server_port = SRV_TCP_P;
	uint16_t blocksize = 1024;
	bool use_nack = false;
	struct sockaddr_in bind_address;
	struct sockaddr_in server_address;

	/* Flags for command line argument parsing */
	int c;
	bool bflag = false, cflag = false, pflag = false, Pflag = false, sflag = false;
	static const char usage[] =
		"Usage: %s [OPTION]... FILE\n"
		"Reference client implementation of the PDP-11 for the COS 540 Project.\n"
		"\n"
		"options:\n"
		"-b\t\tbind to this address (default: all interfaces)\n"
		"-c\t\tthe expected blocksize (default 1024)\n"
		"-h\t\tshow this help message and exit\n"
		"-n\t\tuse negative acknowledgement mode\n"
		"-p\t\tbind to this port (default: automatically assigned port)\n"
		"-P\t\tport server is listening on (default: %"PRIu16")\n"
		"-s\t\taddress of the server (default: %s)\n"
		"\n"
		"The file transmission is completed according to RFC COS540.\n";

	/* Process command line arguments*/
	while ((c = getopt(argc, argv, "b:c:hnp:P:s:")) != -1) {
		switch (c) {
		case 'b':
			if (bflag) {
				fprintf(stderr, "%s: warning: bind address specified multiple times.\n", argv[0]);
			} else {
				bflag = true;
				bind_addr = optarg;
			}
			break;
		case 'c':
			if (cflag) {
				fprintf(stderr, "%s: warning: blocksize specified multiple times.\n", argv[0]);
			} else {
				cflag = true;
				blocksize = parse_port(optarg); /* Not really a port, but fits into about the same range */
				if (errno || blocksize == 0 || blocksize > 4096) {
					fprintf(stderr, ERRCOLOR "%s: error: invalid blocksize: %s\n", argv[0], optarg);
					return 1;
				}
			}
			break;
		case 'h':
			printf(usage, argv[0], SRV_TCP_P, SRV_TCP_A);
			return 0;
		case 'n':
			use_nack = true;
			break;
		case 'p':
			if (pflag) {
				fprintf(stderr, "%s: warning: binding port specified multiple times.\n", argv[0]);
			} else {
				pflag = true;
				bind_port = parse_port(optarg);
				if (errno) {
					fprintf(stderr, ERRCOLOR "%s: error: invalid port specification: %s\n", argv[0], optarg);
					return 1;
				}
			}
			break;
		case 'P':
			if (Pflag) {
				fprintf(stderr, "%s: warning: server port specified multiple times.\n", argv[0]);
			} else {
				Pflag = true;
				server_port = parse_port(optarg);
				if (errno) {
					fprintf(stderr, ERRCOLOR "%s: error: invalid port specification: %s\n", argv[0], optarg);
					return 1;
				}
			}
			break;
		case 's':
			if (sflag) {
				fprintf(stderr, "%s: warning: server address specified multiple times.\n", argv[0]);
			} else {
				sflag = true;
				server_addr = optarg;
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
		fprintf(stderr, "%s: error, no output file specified.\n"
		                "You have to specify the output file name.\n"
				"Try '%s -h' for more information.\n", argv[0], argv[0]);
		return 1;
	}
	file_path = argv[optind];

	/* Process the IP addresses */
	bind_address = parse_address(bind_addr, bind_port);
	if (errno) {
		fprintf(stderr, ERRCOLOR "UDP: %s is an incorrectly specified address.\x1B[0m\n", bind_addr);
		exit(errno);
	}
	server_address = parse_address(server_addr, server_port);
	if (errno) {
		fprintf(stderr, ERRCOLOR "TCP: %s is an incorrectly specified address.\x1B[0m\n", server_addr);
		exit(errno);
	}

	run_transmission(file_path, &bind_address, &server_address, use_nack, blocksize);

	return 0;
}
