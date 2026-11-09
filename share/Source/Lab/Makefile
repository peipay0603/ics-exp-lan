CC = gcc
CFLAGS = -Wall -g
LDLIBS = -lrt
PROGRAMS = lab_udp_ecli.out lab_udp_esrv.out lab_tcp_ecli.out lab_tcp_esrv.out fserver.out fclient.out

.PHONY: all exp43
all: $(PROGRAMS) exp43

%.out: %.c icslab2_net.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

exp43:
	$(MAKE) -C exp43
