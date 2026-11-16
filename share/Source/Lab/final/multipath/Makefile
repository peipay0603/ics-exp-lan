CC = gcc
CFLAGS = -O2 -g -Wall -Wextra -Wpedantic
PROGRAMS = mp_server.out mp_relay.out mp_client.out

.PHONY: all check
all: $(PROGRAMS)

%.out: %.c mp_common.h ../icslab2_net.h
	$(CC) $(CFLAGS) $< -o $@ -lrt

check: all
	python3 test_transfer.py
