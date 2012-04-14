CC=gcc
CFLAGS=

all:
	$(CC) $(CFLAGS) -o server webServer.c

clean:
	rm -f server
