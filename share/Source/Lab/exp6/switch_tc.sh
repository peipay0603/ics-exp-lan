#!/usr/bin/env bash
# ホストPCで実行する。Makefile の tcN と同じ帯域・遅延を、5ノードへほぼ同時に入れ直す。
# 転送中に回線の状況を変えて、経路の切り替えを試すための道具。
#
#   ./switch_tc.sh 3     # 回線設定を tc3 に変える
#   ./switch_tc.sh 2     # tc2 に戻す
#
# make tcN と違い、インタフェース名の並べ替え（fix_eth_order.sh）は行わない（起動時に済んでいる前提）。
# 古い netem が残らないよう、各インタフェースの設定をいったん消してから入れる。
# 1ノードの中では消してから入れ直すまでが一瞬なので、帯域制限のない時間はほぼない。
set -eu
N=${1:-}
case "$N" in
    1|2|3|4) ;;
    *) echo "usage: $0 1|2|3|4" >&2; exit 1 ;;
esac
cd "$(dirname "$0")/../../../.."

pids=""
for node in node1 node2 node3 node4 node5; do
    cmds=$(awk -v t="tc$N:" -v n="$node" '
        $0 == t { f = 1; next }
        /^[^ \t]/ { f = 0 }
        f && $1 == "docker" && $5 == n && $6 == "tc" { $1 = $2 = $3 = $4 = $5 = ""; sub(/^ +/, ""); print }
    ' Makefile)
    if [ -z "$cmds" ]; then
        echo "Makefile に tc$N の $node の設定がありません" >&2
        exit 1
    fi
    script='for d in eth0 eth1 eth2 eth3; do tc qdisc del dev "$d" root 2>/dev/null || :; done'
    docker exec -u 0 "$node" sh -ec "$script
$cmds" &
    pids="$pids $!"
done
status=0
for pid in $pids; do
    wait "$pid" || status=1
done
exit $status
