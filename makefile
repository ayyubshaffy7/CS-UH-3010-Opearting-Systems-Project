# Compiler to use
CC = gcc

# Compiler flags: -g for debugging, -Wall for all warnings
CFLAGS = -g -Wall -pthread

# binaries
TARGETS = myshell server client

# Default rule that builds the target
all: $(TARGETS)

# phase 1 local shell
myshell: main.c utils.c
	$(CC) $(CFLAGS) -o $@ main.c utils.c

# phase 2 server (reuses utils + net)
server: server.c utils.c net.c
	$(CC) $(CFLAGS) -o $@ server.c utils.c net.c

# phase 2 client (just net)
client: client.c net.c
	$(CC) $(CFLAGS) -o $@ client.c net.c

clean:
	rm -f $(TARGETS) *.o *.txt a2 a3 a4 a5 a6 a7 *.log input output