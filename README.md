COS 540 Project
===============
Ryan Kinney - Spring 2024

This project describes and implements a high-performance, multi-threaded,
application-layer, and reliable communication protocol. Two threads are used
for the file transfer, one uses UDP for data transmission, and the other uses
TCP for control information. It uses the client-server paradigm, and the
communication protocol is described in the attached file, [RFC.txt](./RFC.txt).

The end of this document contains an analysis of efficiency. The first part talks
about blocksizes, and the second part talks about performance ofthe acknowledgement
modes.

Requirements
------------
The basic requirements for the protocol are as follows:
- The protol is implemented with the client/server paradigm.
- The client must exactly recreate the file sent by the server.
- The implementation must include a multi-threaded client and server
  and use separate threads for the UDP and TCP sockets (at both hosts).
- Files are transmitted in chunks of up to 4096 bytes.
- Chunks are numbered from {0 .. Total_Chunks - 1}, representing their
  position within the original data file.
- Each chunk consists of a 32-bit integer header specifying its chunk number,
  immediately followed by the data payload.
- TCP is used to exchange control information, including initialization and
  on-going control information.
- UDP is used for data transfer.
- Reliability is provided by an acknowledgment retransmission method. Both
  Selective Acknowledgements (SACK) and Negative Acknowledgements (NACK) must be
  implemented.


The code for the client is located in [client.c](./client.c) and the server is
located in [server.c](./server.c). Some utility functions are located in
[common.c](./common.c) and [common.h](./common.h). Finally, data structures for
the packets are located in [packets.h](packets.h).

Configuration
-------------
The IP addresses and ports the programs bind to can be specified at runtime with
command line arguments. If command line arguments are not supplied, then the
defaults are used. The values of the defaults are set with the following macros
in both the client and server source files:
- `SRV_TCP_A`
  - Server bind address, a string.
- `SRV_TCP_P`
  - Server bind port, an integer.
- `CLI_UDP_A`
  - Client UDP bind address, a string.
- `CLI_UDP_P`
  - Client UDP bind port, an integer.
- `CLI_TCP_A`
  - Client TCP bind address, a string (currently unused).
- `CLI_TCP_P`
  - Client TCP bind port, an integer (currently unused).

Additionally, `FILE_SIZE_MAX` is defined for the maximum file size of the transfer.
This macro is only located in the server source file and is not overridable by
command line arguments. The default is `17592186044416` bytes (2^32 blocks of
4096 bytes), which is the upper bound for file size supported by the defined
protocol. If `FILE_SIZE_MAX` is set larger than the default, and the attempted
file transfer is larger than `17592186044416` bytes, error messages will be
printed, and no file transfer will occur.

Installation
------------
To build the client and server programs, simply run `make` in the top-level
directory of the project (where the source files and `Makefile` live) The
`client` and `server` binaries are created in the same directory as the source files.

Usage
-----

### Server

The most basic way to run the server program is to use the defaults for the IP
address and port, and provide a file name to transfer. To transfer the file,
`file.txt`, and bind to the default IP address and port, run:
```sh
./server file.txt
```

To run specify the bind address and port, the `-b` and `-p` options are used:
```sh
./server -b 0.0.0.0 -p 8888 file.txt
```

For more information, a help message is shown with `./server -h`.

### Client
The most basic way to run the server program is to use the default
configuration and provide the file name the data is written to.
To run with the default configuration:
```sh
./client received.txt
```

To specify the address and port of the server to connect to, the options, `-s`
and `-P` are used. Additionally, the client can specify the wanted blocksize
with the `-c` option, and the ACK mode (selective/negative) with the `-n`
option. To connect to the server bound to the IP address and port, `192.168.68.65`
and `8080`, use a blocksize of 4096 bytes, and use negative acknowledgements the
following is used:
```sh
./client -s 192.168.68.65 -P 8080 -c 4096 -n received.txt
```

Additionally the client can specify the IP address and port its UDP socket is
bound to with the `-b` and `-p` options. For more information, a help message
is shown with `./client -h`.

Testing and Efficiency Analysis
-------------------------------
The protocol was tested with various different configurations on different
systems. Initial development was performed on a single machine (server listening
on a loopback address). Once that was working, systems connected on a LAN were
tested. Next, I tested the protocol on my local machine and the ACG VM. Finally,
it was tested between the ACG VM and aturing. Additionally, different blocksizes
and SACK vs NACK modes were tested for efficiency. For testing, I used a 1 GB
file of binary data for the transfer.

I first tested the effect of blocksize on transfer speed, then on packet loss.
Finally, I tested the efficiency differences between SACK and NACK modes.

### Blocksize Analysis
----------------------
The blocksize effects both the overall transfer speed of the protocol (assuming
no packet loss) as well as the number of packets being dropped.


#### Transfer Speed
The blocksize had a huge impact on the performance of the protocol. In theory,
larger blocksizes should lead to  transfer speeds due to less overhead for the
routers and systems. On my local machines, I tested with 1 GB file and used
blocksizes of 32, 64, 128, 256, 512, 1024, 2048, and 4096 bytes. I measured the
time the client took to connect to the server, receive the file, and exit.

**Table 1**: Transfer time of a 1 GB file for various blocksizes over a dedicated
             1 Gb/s link between two systems. No packets were harmed (dropped) in
             the production of this data.

| Blocksize | Total Number of Blocks | Real Time |
|-----------|------------------------|-----------|
|     32    |        33554432        | 1m35.035s |
|     64    |        16777216        | 0m51.071s |
|    128    |         8388608        | 0m28.896s |
|    256    |         4194304        | 0m17.316s |
|    512    |         2097152        | 0m10.871s |
|   1024    |         1048576        | 0m09.271s |
|   2048    |          524288        | 0m09.221s |
|   4096    |          262144        | 0m09.075s |

A larger blocksize lead to a faster transfer rate, up to a certain point, and this
is mostly due to overhead in both the data structure and with the number of packets
being transferred. At very low blocksizes (such as 1 byte), there is also massive
overhead in the reading and writing of the files themselves on the systems. As the
blocksize increases and the amount of overhead per block decreases, the main
bottleneck is simply the link speed instead of all the overhead. This is observed in
Table 1, as the transfer time seems to stop increasing much once the blocksize
hit 1024.

#### Packet Loss
To investigate packet loss, connections with multiple hops are needed. To
achieve this, I performed a test between my local machine and the ACG VM, as well
as a test between aturing and the ACG VM. I used the SACK acknowledgement mode
for each of the tests. I used blocksizes of 512, 1024, 1400, 1500, 2048, and 4096
bytes. In all cases, the ACG VM acted as the client, and the other machine was
the server.

**Table 2**: Transfer times of a 1 GB file for various blocksizes between my local
             machine and the ACG VM. Unfortunately, packets were harmed (dropped)
             in the production of the data.

| Blocksize | Total Number of Blocks |   Real Time   |
|-----------|------------------------|---------------|
|    512    |         2097152        |   1m31.034s   |
|   1024    |         1048576        |   0m51.277s   |
|   1400    |          766959        |   0m34.531s   |
|   1500    |          715828        | Didn't finish |
|   2048    |          524288        | Didn't Finish |
|   4096    |          262144        | Didn't Finish |

For large blocksizes, the number of packets dropped was large, to the point where
the final blocks were always dropped and the file transfer never completed (my
implementation gives up after 25 rounds in a row with all blocks dropped). With
lower blocksizes, less packets were dropped, and the file transfer was able to
finish. I believe the cause of this is due to fragmentation and the MTU of the
routers. For example, the router between my local machine and the public internet
has an MTU of around 1450 bytes, and many times UDP packets above that size get
dropped, which was observed in the measurements in Table 2. If a single fragment
of the UDP packet gets dropped, the entire packet is dropped, so a smaller
blocksize, which has less fragmentation, allows the transfer to complete
efficiently. For maximum performance, a small blocksize that strikes a balance
between transfer speed and packet loss needs to be chosen.

**Table 3**: Transfer times of a 1 GB file for various blocksizes between
             aturing and the ACG VM. Unfortunately, packets were harmed.
             (dropped) in the production of this data.

| Blocksize | Total Number of Blocks |   Real Time   |
|-----------|------------------------|---------------|
|    512    |         204800         |   1m34.248s   |
|   1024    |         102400         |   0m48.348s   |
|   2048    |         524288         |   0m25.661s   |
|   4096    |         262144         |   0m14.259s   |

Between aturing the ACG VM and aturing, the larger blocksizes did not cause the
transfer to fail, and significantly sped up the transfer speed. The main
bottleneck here is the acknowledgement mode used in the transfer.

### Acknowledgement Mode Performance
------------------------------------
The protocol supports both SACK and NACK reliability modes, and theoretically
supports dynamically switching between the two, however the reference client
implementation does not support dynamically changing the acknowledgement mode.
Both SACK and NACK have benefits and downsides. One downside to these methods
is the amount of data that needs to be transmitted from the client to the server
scales with the number of blocks in the file transfer.

The SACK implementation includes a bitmap where each bit is the status of a
single block, meaning the size of the SACK packet is fixed throughout the
transmission and its length in bits is approximately the number of blocks in
the file (num_blocks/8 bytes). For simpler implementation, the size of the SACK
is rounded up to the nearest 32 bit word. The main benefit SACK has over the NACK
mode is when there is a high amount of packet loss, there is less data the client
needs to send to the server.

The NACK implementation has the client send the 32 bit index for every block
that was missing in order. Therefore, the NACK length ranges from 4 bytes to
num_blocks * 4 bytes in length. The benefit to NACK is when there are few packets
missing, then there is less data the client needs to send to the server for the
acknowledgement when compared to the SACK mode.

**Table 4**: Transfer times of a 1 GB file for various blocksizes between
             aturing and the ACG VM for NACK and SACK modes. Unfortunately,
             packets were harmed. (dropped) in the production of this data.

| Blocksize | Total Number of Blocks |   SACK Time   |   NACK Time   |
|-----------|------------------------|---------------|---------------|
|    512    |         2097152        |   1m34.248s   |   1m33.537s   |
|   1024    |         1048576        |   0m48.348s   |   0m48.646s   |
|   2048    |          524288        |   0m25.661s   |   0m25.159s   |
|   4096    |          262144        |   0m14.259s   |   0m13.427s   |

As can be seen in Table 4, there is no large difference in the performance
of SACK or NACK modes. This is due to when my implementation sends the
acknowledgement, which is at the end of a round. In earlier rounds, a relatively
large amount of packets are missing, so the SACK smaller than the equivalent NACK.
In later rounds, when fewer blocks are missing, the NACK is smaller, and the time
difference begins to balance out. Ideally, the client would switch to NACK mode
when the number of missing blocks is less than num_blocks/32 rounded up, which
would get the benefits of both SACK and NACK and bypass the tradeoffs between the
two.
