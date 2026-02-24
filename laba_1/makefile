all: server client

server: server.cpp
	g++ -std=c++11 -Wall -o server.exe server.cpp

client: client.cpp
	g++ -std=c++11 -Wall -o client.exe client.cpp

clean:
	rm -f server client

.PHONY: all clean