CC=gcc
CFLAGS=-g -Wall

all: dedicated

dedicated: src/dedicated.c src/net.c src/net.h
	$(CC) $(CFLAGS) -o cncnet-dedicated src/dedicated.c src/net.c $(LIBS)

clean:
	rm -f cncnet-dedicated
