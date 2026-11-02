#
#	FILENAME    : Makefile
#	Description : Makefile for BStat test
#

CC		= gcc
CFLAGS		= -g -Wall -DDEBUG
LD		= gcc
LDFLAGS 	= -lm -lrt
TAR		= tar
RM		= /bin/rm
CP		= /bin/cp
ZIP		= zip

ALL_SRCS		= tcp_echo_server.c tcp_echo_client.c udp_echo_server.c udp_echo_client.c tcp_file_server.c tcp_file_client.c udp_echo_client_ep.c sample_clock_gettime.c  sample_epoll.c
ALL_HEADERS 	= $(COMMON_HEADERS) $(OTHERS)
ALL_OBJS		= tcp_echo_server.o tcp_echo_client.o udp_echo_server.o udp_echo_client.o tcp_file_server.o tcp_file_client.o udp_echo_client_ep.o sample_clock_gettime.o  sample_epoll.o
ALL_EXES		= tcp_echo_server.out tcp_echo_client.out udp_echo_server.out udp_echo_client.out tcp_file_server.out tcp_file_client.out udp_echo_client_ep.out sample_clock_gettime.out  sample_epoll.out

COMMON_HEADERS		= icslab2_net.h
OTHERS			= Makefile

all	: $(ALL_EXES)

.c.out	:
	gcc $(CFLAGS) $< -o $@ $(LDFLAGS)

.c.o	:
	gcc -c $<

clean :
	-$(RM) -f $(ALL_OBJS) $(ALL_EXES) *.out test.dat *~ Lab/*~ Lab/*.out
	-$(RM) -rf ./work GPATH GRTAGS GTAGS

archive :
	(cd ..; $(TAR) cvfz icslab2_net.tar.gz Source)

zip	:
	(cd ..; $(ZIP) -r icslab2_net.zip Source)

nlsrc	:
	mkdir -p ./work
	nl -s":  " -ba udp_echo_client.c | sed -e 's/^    //g'  > work/src_udp_echo_client.txt
	nl -s":  " -ba udp_echo_server.c | sed -e 's/^    //g'  > work/src_udp_echo_server.txt 
	nl -s":  " -ba tcp_echo_client.c | sed -e 's/^    //g'  > work/src_tcp_echo_client.txt
	nl -s":  " -ba tcp_echo_server.c | sed -e 's/^    //g'  > work/src_tcp_echo_server.txt

test.dat	:
	head -c 1m /dev/urandom > test.dat
