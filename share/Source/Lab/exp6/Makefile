CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -D_GNU_SOURCE
LDLIBS = -lrt
PROGRAMS = tcp_send.out tcp_recv.out hyb_send.out hyb_recv.out udp_relay.out

# make data SIZE=100 で 100MiB のテストファイル test100M.dat を作る
SIZE ?= 100

.PHONY: all clean data
all: $(PROGRAMS)

%.out: %.c common.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

data: test$(SIZE)M.dat

test%M.dat:
	head -c $$(( $* * 1024 * 1024 )) /dev/urandom > $@

clean:
	rm -f $(PROGRAMS)
