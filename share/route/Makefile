# 最終課題（高速ファイル転送）
#   make            … fserver.out / fclient.out / relay.out をビルド
#   make test.dat   … 100 MiB のテストファイル（SIZE=900 などで変更）
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDLIBS  = -lpthread -lm
SIZE   ?= 100

PROGS = fserver.out fclient.out relay.out

all: $(PROGS)

fclient.out: output_file.h

test: all
	python3 -m unittest -v test_transfer test_routeplan test_bench

%.out: %.c common.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

test.dat:
	head -c $$(( $(SIZE) * 1024 * 1024 )) /dev/urandom > $@

clean:
	rm -f $(PROGS)

.PHONY: all clean test
