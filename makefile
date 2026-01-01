client: client.cpp
	g++ -std=c++11 -Wall -Wextra -g3 client.cpp -o client

server: server.cpp
	g++ -Wall -Wextra -g3 server.cpp -o server

.PHONY: clean
clean:
	rm -f client
	rm -f server
