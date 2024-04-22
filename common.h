#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "packets.h"

/*
 * Extracts and validates the port number stored in a port_str.
 * Returns 0 and updates errno on error.
 */
uint16_t parse_port(const char *port_str);

/* Updates errno on error */
struct sockaddr_in parse_address(const char *ip_address, uint16_t port);

extern inline void set_block_status(uint32_t idx, ACKPacket_t *sack);
extern inline bool get_block_status(uint32_t idx, const ACKPacket_t *sack);

#endif /* COMMON_H */