#!/usr/bin/env bash
# ホストPCで明示的に実行する。測定中には実行しないこと。
# 同じhandleでtbfをreplaceすると以前のnetemが残るため、一度rootを削除する。
set -euo pipefail
cd "$(dirname "$0")/../../../.."
for node in node1 node2 node3 node4 node5; do
    docker exec -u 0 "$node" sh -ec '
        for dev in eth0 eth1 eth2 eth3; do
            state=$(tc qdisc show dev "$dev")
            case "$state" in
                *"qdisc noqueue 0:"*) ;;
                *) tc qdisc del dev "$dev" root ;;
            esac
        done
    '
done
make tc2
