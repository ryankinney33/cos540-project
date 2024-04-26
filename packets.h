#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* Constants used in the control packet header */
typedef enum PType {
	PTYPE_UDPRDY,
	PTYPE_FILEINFO,
	PTYPE_SACK,
	PTYPE_NACK,
	PTYPE_COMPLETE
} __attribute__((packed)) PType_t;

/* Control Packet Definitions */
typedef struct ControlHeader { /* PDP-11 Control Packet Header */
	char head[3]; /* Will contain "PDP" */
	PType_t type; /* The type of control packet */
} __attribute__((packed)) ControlHeader_t;

typedef struct FileInformationPacket {
	ControlHeader_t header; /* PDP-11 Control Packet Header */
	uint32_t num_blocks; /* Number of blocks in the file - 1*/
	uint16_t blocksize; /* Number of bytes in a block - 1*/
} __attribute__((packed)) FileInformationPacket_t;

typedef struct ACKPacket {
	ControlHeader_t header; /* PDP-11 Control Packet Header */
	uint32_t length; /* Number of elements in the ack_stream - 1 */
	uint32_t ack_stream[]; /* The SACK or the NACK */
} __attribute__((packed)) ACKPacket_t;

typedef ControlHeader_t CompletePacket_t; /* Complete Packet */
typedef ControlHeader_t UDPReadyPacket_t; /* UDP Ready Packet */

/* The default Control Packet Headers */
#define CONTROL_HEADER_DEFAULT {.head={'P','D','P'}, .type=PTYPE_COMPLETE}
#define UDP_READY_INITIALIZER {.head={'P', 'D', 'P'}, .type=PTYPE_UDPRDY}

/* Data Packet Definition */
typedef struct FileBlockPacket {
	uint32_t index; /* Block index*/
	unsigned char data_stream[]; /* File data */
} __attribute__((packed)) FileBlockPacket_t;

#endif /* PROTOCOL_H */