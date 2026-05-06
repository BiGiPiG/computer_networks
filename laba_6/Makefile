CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread -g
LDFLAGS = -pthread -lm

all: server client

server: server.c utils.c message.h
	$(CC) $(CFLAGS) -o server.exe server.c utils.c $(LDFLAGS)

client: client.c utils.c message.h
	$(CC) $(CFLAGS) -o client.exe client.c utils.c $(LDFLAGS)

clean:
	rm -f server.exe client.exe

.PHONY: all clean
