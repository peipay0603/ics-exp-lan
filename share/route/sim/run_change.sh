#!/bin/bash
# run_change.sh -- 転送中にネットワークの設定が変わる場合の比較（sim_mesh.sh のネットワーク上）
#
#   sudo ./sim_mesh.sh up 2
#   sudo ./run_change.sh [変更後のtc番号] [変更する秒] [サイズMiB]     例: ./run_change.sh 3 5 100
#   変更後の番号に 2 を指定すると「変更なし」
#
# 経路は最初の設定（tc2）で routeplan.py が決め，転送開始から指定秒後に tcN に切り替える．
#   D_static_single    : 3→4→5→1 だけ・経路固定（-S）
#   G_adaptive_single  : 3→4→5→1 から始め，速度を見て経路を変更（ほかの 9 経路は予備）
#   E_static_multi     : 最大流の 4 経路・経路固定（-S）
#   F_adaptive_multi   : 最大流の 4 経路から始め，速度を見て経路を変更（6 経路は予備）
set -u
cd "$(dirname "$0")/.."            # share/final
TO=${1:-3}
AT=${2:-5}
SIZE=${3:-100}
LIMIT=${LIMIT:-120}                # これ以上かかったら打ち切る [秒]
RES=sim/results/change_tc2_to_tc$TO
mkdir -p $RES logs

make -s
[ -f test.dat ] && [ $(stat -c %s test.dat) -eq $((SIZE * 1024 * 1024)) ] || make -s test.dat SIZE=$SIZE
SUM=$(md5sum < test.dat | cut -d' ' -f1)

# 最初の設定（tc2）で調査して計画する
sim/sim_mesh.sh apply 2 > /dev/null
rm -rf links
for i in 1 2 3 4 5; do ip netns exec node$i ./collect_links.sh > /dev/null; done
python3 routeplan.py > $RES/plan.txt

pkill -x fserver.out; pkill -x relay.out; sleep 0.3
for n in 2 3 4 5; do ip netns exec node$n plan/node$n.sh > /dev/null; done
sleep 0.5

MULTI=$(grep -o '\./fclient.out .*' plan/node1.sh)
# 単一経路版：最初の使用経路（3→4→5→1）だけを使用中にし，残りを予備にする
SINGLE=$(python3 - "$MULTI" <<'EOF'
import sys
args = sys.argv[1].split()
head, paths, sby = [], [], []
i = 0
while i < len(args):
    a = args[i]
    if a == "-s":
        sby.append(args[i + 1]); i += 2; continue
    if ":" in a and "=" in a:
        paths.append(a)
    else:
        head.append(a)
    i += 1
first, rest = paths[0], paths[1:]
sby = [p.split("@")[0] + "=" + p.split("=")[1] for p in rest] + sby
print(" ".join(head + sum([["-s", s] for s in sby], []) + [first]))
EOF
)
FIRST=$(echo "$MULTI" | grep -o '172[^ ]*=3-4-5-1' | head -1)

summary=$RES/summary.txt
echo "file $SIZE MiB, plan from tc2, network changes to tc$TO at t=${AT}s (bandwidth only)" > $summary

run() {   # $1=label $2...=command
    local label=$1; shift
    sim/sim_mesh.sh apply 2 > /dev/null
    sleep 1
    echo "=== $label: $*" > $RES/$label.log
    local t0=$(date +%s.%N)
    timeout $LIMIT ip netns exec node1 "$@" >> $RES/$label.log 2>&1 &
    local pid=$!
    if [ "$TO" != 2 ]; then
        sleep $AT
        sim/sim_mesh.sh apply $TO > /dev/null
        echo "[network changed to tc$TO at about ${AT}s]" >> $RES/$label.log
    fi
    wait $pid
    local rc=$?
    local t1=$(date +%s.%N)
    if [ $rc -ne 0 ]; then
        printf "%-20s timeout (> %d s)\n" "$label" "$LIMIT" | tee -a $summary
    else
        local wall=$(echo "$t1 - $t0" | bc)
        local mbps=$(echo "scale=2; $SIZE * 1048576 * 8 / $wall / 1000000" | bc)
        local s=$(md5sum < received.dat | cut -d' ' -f1)
        local ok=$([ "$s" = "$SUM" ] && echo "md5 OK" || echo "md5 MISMATCH")
        printf "%-20s %7.2f s  %7.2f Mbps  %s\n" "$label" "$wall" "$mbps" "$ok" | tee -a $summary
    fi
    rm -f received.dat
    sleep 1
}

run D_static_single   ./fclient.out -S -o received.dat ${FIRST%%@*}
run G_adaptive_single $SINGLE
run E_static_multi    $MULTI -S
run F_adaptive_multi  $MULTI

pkill -x fserver.out; pkill -x relay.out
sim/sim_mesh.sh apply 2 > /dev/null
