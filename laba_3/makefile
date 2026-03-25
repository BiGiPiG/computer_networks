CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread -g
LDFLAGS = -pthread

all: server client

server: server.c utils.c message.h
	$(CC) $(CFLAGS) -o server server.c utils.c $(LDFLAGS)

client: client.c utils.c message.h
	$(CC) $(CFLAGS) -o client client.c utils.c $(LDFLAGS)

clean:
	rm -f server client

.PHONY: all clean