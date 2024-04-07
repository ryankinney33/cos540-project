
CFLAGS := -Wall -Wextra -g -pipe

.PHONY: all clean

all: server client

server: server.c
client: client.c

clean:
	$(RM) server client