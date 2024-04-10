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
} PType_t;

/* Control Packet Definitions */
typedef struct ControlHeader {
	char head[3]; /* Will contain "PDP" */
	PType_t type: 8;
} ControlHeader_t;

typedef struct UDPInformationPacket {
	ControlHeader_t header;
	uint16_t destination_port;
} UDPInformationPacket_t;

typedef struct FileInformationPacket {
	ControlHeader_t header;
	uint32_t num_blocks;
	uint16_t blocksize: 12;
} FileInformationPacket_t;

typedef struct ACKPacket {
	ControlHeader_t header;
	uint32_t length;
	uint32_t ack_stream[];
} ACKPacket_t;

typedef ControlHeader_t CompletePacket_t;

#define CONTROLHEADER_DEFAULT {.head={'P','D','P'}, .type=COMPLETE}

#endif /* PROTOCOL_H */