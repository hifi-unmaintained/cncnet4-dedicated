CC=gcc
CFLAGS=-g -Wall

all: dedicated

dedicated: src/dedicated.c src/net.c src/net.h
	$(CC) $(CFLAGS) -o cncnet-dedicated src/dedicated.c src/net.c

win32: src/dedicated.c src/net.c src/net.h
	i586-mingw32msvc-gcc $(CFLAGS) -o cncnet-dedicated.exe src/dedicated.c src/net.c -lws2_32

clean:
	rm -f cncnet-dedicated cncnet-dedicated.exe
