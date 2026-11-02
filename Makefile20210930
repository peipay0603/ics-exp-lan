.PHONY: build clean

COMPOSE := docker-compose

build: setenv
	${COMPOSE} build
start1: setenv
	${COMPOSE} up -d
	$(MAKE) tc1 # 1巡目
start2: setenv
	${COMPOSE} up -d
	$(MAKE) tc2 # 2巡目
start3: setenv
	${COMPOSE} up -d
	$(MAKE) tc3 # 3巡目
start4: setenv
	${COMPOSE} up -d
	$(MAKE) tc4 # 4巡目
setenv:
	> .env
	echo "UID=$(shell id -u)" >> .env
	echo "GID=$(shell id -u)" >> .env # Set GID using UID (For Mac)
stop:
	${COMPOSE} stop
ps:
	${COMPOSE} ps
log:
	${COMPOSE} logs -f
clean:
	${COMPOSE} down --rmi all
cleanall: clean
	-docker container stop $(shell docker container ls -aq)
	-docker system prune --volumes -af

login-node1:
	docker exec -it node1 bash
login-node2:
	docker exec -it node2 bash
login-node3:
	docker exec -it node3 bash
login-node4:
	docker exec -it node4 bash
login-node5:
	docker exec -it node5 bash

tc1:
	docker exec -u 0 node1 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node1 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node1 tc qdisc replace dev eth1 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node1 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node1 tc qdisc replace dev eth2 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node1 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 250ms
	docker exec -u 0 node1 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node2 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node2 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node2 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node2 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node3 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node3 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node3 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node3 tc qdisc replace dev eth2 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node3 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node3 tc qdisc replace dev eth3 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node3 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth0 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node4 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 250ms
	docker exec -u 0 node4 tc qdisc replace dev eth1 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node4 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth2 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node4 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node4 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node5 tc qdisc replace dev eth0 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node5 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node5 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node5 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node5 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node5 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 5ms
tc2:
	docker exec -u 0 node1 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node1 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node1 tc qdisc replace dev eth1 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node1 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 2.5ms
	docker exec -u 0 node1 tc qdisc replace dev eth2 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node1 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node1 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node2 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node2 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth1 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node2 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node2 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth3 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node3 tc qdisc replace dev eth0 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node3 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 2.5ms
	docker exec -u 0 node3 tc qdisc replace dev eth1 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node3 tc qdisc replace dev eth2 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node3 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node3 tc qdisc replace dev eth3 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node3 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth0 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node4 tc qdisc replace dev eth1 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node4 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth2 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node4 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node4 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node4 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node5 tc qdisc replace dev eth0 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node5 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node5 tc qdisc replace dev eth1 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node5 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node5 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node5 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node5 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 10ms
tc3:
	docker exec -u 0 node1 tc qdisc replace dev eth0 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node1 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node1 tc qdisc replace dev eth1 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node1 tc qdisc replace dev eth2 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node1 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 2.5ms
	docker exec -u 0 node1 tc qdisc replace dev eth3 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node1 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth0 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node2 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node2 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node2 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node2 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node2 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth3 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node3 tc qdisc replace dev eth0 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node3 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node3 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node3 tc qdisc replace dev eth2 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node3 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node3 tc qdisc replace dev eth3 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node3 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth0 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node4 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 2.5ms
	docker exec -u 0 node4 tc qdisc replace dev eth1 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node4 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth2 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node4 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node4 tc qdisc replace dev eth3 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node5 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node5 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node5 tc qdisc replace dev eth1 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node5 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node5 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node5 tc qdisc replace dev eth3 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
tc4:
	docker exec -u 0 node1 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node1 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node1 tc qdisc replace dev eth1 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node1 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 2.5ms
	docker exec -u 0 node1 tc qdisc replace dev eth2 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node1 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node1 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node2 tc qdisc replace dev eth0 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node2 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node2 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node2 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node2 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node2 tc qdisc replace dev eth3 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node2 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node3 tc qdisc replace dev eth0 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node3 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 2.5ms
	docker exec -u 0 node3 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node3 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node3 tc qdisc replace dev eth2 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node3 tc qdisc replace dev eth3 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node3 tc qdisc replace dev eth3 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth0 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node4 tc qdisc replace dev eth1 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node4 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node4 tc qdisc replace dev eth2 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node4 tc qdisc replace dev eth3 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
	docker exec -u 0 node5 tc qdisc replace dev eth0 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node5 tc qdisc replace dev eth0 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node5 tc qdisc replace dev eth1 root handle 1: tbf rate 50mbit burst 25kb limit 250kb
	docker exec -u 0 node5 tc qdisc replace dev eth1 parent 1:1 handle 10: netem delay 10ms
	docker exec -u 0 node5 tc qdisc replace dev eth2 root handle 1: tbf rate 1mbit burst 10kb limit 100kb
	docker exec -u 0 node5 tc qdisc replace dev eth2 parent 1:1 handle 10: netem delay 5ms
	docker exec -u 0 node5 tc qdisc replace dev eth3 root handle 1: tbf rate 10mbit burst 20kb limit 200kb
