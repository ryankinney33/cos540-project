#include <stdio.h>

#include <pthread.h>

#define SRV_TCP_A   "127.0.0.1"
#define SRV_TCP_P   27020
#define CLI_UDP_A   "0.0.0.0"
#define CLI_UDP_P   25567

/* These are extraneous... */
#define CLI_TCP_A   "0.0.0.0"
#define CLI_TCP_P   27021

int main() {
	printf("Client!\n");
	return 0;
}