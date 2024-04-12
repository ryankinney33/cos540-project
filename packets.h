#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* Constants used in the control packet header */
typedef enum PType {
	UDPINFO,
	FILEINFO,
	SACK,
	NACK,
	COMPLETE
} __attribute__((packed)) PType_t;

/* Control Packet Definitions */
typedef struct ControlHeader { /* PDP-11 Control Packet Header */
	char head[3]; /* Will contain "PDP" */
	PType_t type; /* The type of control packet */
} __attribute__((packed)) ControlHeader_t;

typedef struct UDPInformationPacket {
	ControlHeader_t header; /* PDP-11 Control Packet Header */
	uint16_t destination_port; /* Port the client is listening on */
} __attribute__((packed)) UDPInformationPacket_t;

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

/* The default Control Packet Header */
#define CONTROL_HEADER_DEFAULT {.head={'P','D','P'}, .type=COMPLETE}

/* Data Packet Definition */
typedef struct FileBlockPacket {
	uint32_t index; /* Block index*/
	unsigned char data_stream[]; /* File data */
} __attribute__((packed)) FileBlockPacket_t;

#endif /* PROTOCOL_H */