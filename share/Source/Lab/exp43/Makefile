CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
ifeq ($(shell uname -s),Linux)
LDLIBS += -lrt
endif

.PHONY: all clean compare
all: fserver.out fclient.out

fserver.out: fserver.c common.h ../icslab2_net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) fserver.c -o $@ $(LDLIBS)

fclient.out: fclient.c common.h ../icslab2_net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) fclient.c -o $@ $(LDLIBS)

# 資料の1MBテストファイル。実際の転送バイト数でグッドプットを求める。
test.dat:
	head -c 1048576 /dev/urandom > $@

compare: all test.dat
	python3 compare_local.py

clean:
	rm -f fserver.out fclient.out
