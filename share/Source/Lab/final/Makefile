CC = gcc
CFLAGS = -Wall -Wextra -g
LDLIBS = -lrt
PROGRAMS = fserver.out fclient.out relay.out baseline-server.out baseline-client.out

.PHONY: all multipath
all: $(PROGRAMS) multipath

multipath:
	$(MAKE) -C multipath

fserver.out: fserver.c icslab2_net.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
fclient.out: fclient.c icslab2_net.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
relay.out: relay.c icslab2_net.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
baseline-server.out: fserver.c icslab2_net.h
	$(CC) $(CFLAGS) -DBUF_LEN=1000 $< -o $@ $(LDLIBS)
baseline-client.out: fclient.c icslab2_net.h
	$(CC) $(CFLAGS) -DBUF_LEN=1000 $< -o $@ $(LDLIBS)
