CFLAGS := -O2 -Wall -Wextra -g -std=gnu99
LDLIBS := -lpthread

.PHONY: all clean
all: server client

server: server.o common.o
client: client.o common.o

clean:
	$(RM) server client *.o
