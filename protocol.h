#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* Constants used in the control packets*/
typedef enum PType {
	UDPINFO,
	FILEINFO,
	SACK,
	NACK,
	COMPLETE
} __attribute__((packed)) PType_t;

/* Control Packet Definitions */
typedef struct ControlHeader {
	char head[3]; /* Will contain "PDP" */
	PType_t type;
} __attribute__((packed)) ControlHeader_t;

typedef struct UDPInformationPacket {
	ControlHeader_t header;
	uint16_t destination_port;
} __attribute__((packed)) UDPInformationPacket_t;

typedef struct FileInformationPacket {
	ControlHeader_t header;
	uint32_t num_blocks;
	uint16_t blocksize;
} __attribute__((packed)) FileInformationPacket_t;

typedef struct ACKPacket {
	ControlHeader_t header;
	uint32_t length;
	uint32_t ack_stream[];
} __attribute__((packed)) ACKPacket_t;

typedef ControlHeader_t CompletePacket_t;

#define CONTROL_HEADER_DEFAULT {.head={'P','D','P'}, .type=COMPLETE}

#endif /* PROTOCOL_H */