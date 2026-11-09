CC = gcc
CFLAGS = -Wall -Wextra -g
LDLIBS = -lrt

.PHONY: all clean
all: fserver.out fclient.out

fserver.out: fserver.c icslab2_net.h
	$(CC) $(CFLAGS) fserver.c -o $@ $(LDLIBS)

fclient.out: fclient.c icslab2_net.h
	$(CC) $(CFLAGS) fclient.c -o $@ $(LDLIBS)

test.dat:
	head -c 1048576 /dev/urandom > $@

clean:
	rm -f fserver.out fclient.out
