COS 540 Project
===============
Ryan Kinney - Spring 2024

This project describes and implements a high-performance, multi-threaded,
application-layer, and reliable communication protocol. Two threads are used
for the file transfer, one uses UDP for data transmission, and the other uses
TCP for control information. It uses the client-server paradigm, and the
communication protocol is described in the attached file, [RFC.txt](./RFC.txt).

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

Testing and Analysis
--------------------
The protocol was tested with various different configurations on different
systems. Initial development was performed on a single machine (server listening
on a loopback address). Once that was working, systems connected on a LAN were
tested. Next, I tested the protocol on my local machine and the ACG VM. Finally,
it was tested between the ACG VM and aturing. Additionally, different blocksizes
and SACK vs NACK modes were tested for efficiency. For file sizes, I used a 1 GB
file of binary data with my local machines. Additionally, I used a 100 MB file on
aturing, due to disk space problems, likely caused by other users not deleting
their test files.

I first tested the effect of blocksize on transfer speed, then on packet loss.
Finally, I tested the efficiency differences between SACK and NACK modes.

### Blocksize
-------------
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
