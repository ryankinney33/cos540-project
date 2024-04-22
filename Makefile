CFLAGS := -Wall -Wextra -g -pipe -std=gnu99
LDLIBS := -lpthread

.PHONY: all clean
all: server client

server: server.o common.o
client: client.o common.o

clean:
	$(RM) server server.o client client.o common.o
