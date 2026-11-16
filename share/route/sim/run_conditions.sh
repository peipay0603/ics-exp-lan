#!/bin/bash
# run_conditions.sh -- 比較実験 A〜E を順に実行する（sim_mesh.sh で作ったネットワーク上）
#
#   sudo ./sim_mesh.sh up 2
#   sudo ./run_conditions.sh [サイズMiB]      （既定 100）
#
#   A 変更なし          : 実験4-1(exp43) の fserver/fclient（BUF_LEN=1000）で直接 3→1
#   B 経路だけ変更      : exp43 のまま，relay.out で 3→4→5→1 を通す
#   C プログラムだけ変更: 新しい fserver/fclient（sendfile, 1MiB 単位）で直接 3→1
#   D 両方変更          : 新しい fserver/fclient で 3→4→5→1
#   E 複数経路          : routeplan.py の最大流の分解（5 Mbps 以上の 4 経路）を同時に使う
set -u
cd "$(dirname "$0")/.."            # share/final
SIZE=${1:-100}
EXP43=../Source/Lab/exp43
RES=sim/results
mkdir -p $RES logs
B43=$(mktemp -d)

make -s
gcc -O2 -o $B43/fserver43.out $EXP43/fserver.c
gcc -O2 -o $B43/fclient43.out $EXP43/fclient.c
[ -f test.dat ] && [ $(stat -c %s test.dat) -eq $((SIZE * 1024 * 1024)) ] || make -s test.dat SIZE=$SIZE
SUM=$(md5sum < test.dat | cut -d' ' -f1)

stop_all() { pkill -x fserver.out; pkill -x relay.out; pkill -x fserver43.out; sleep 0.5; }

# routeplan.py の結果どおりに node2〜5 のスクリプトを各 namespace で起動する
start_plan() {
    for n in 2 3 4 5; do
        [ -f plan/node$n.sh ] && ip netns exec node$n plan/node$n.sh > /dev/null
    done
    sleep 0.5
}

check() {   # $1=label $2=file
    local s=$(md5sum < "$2" | cut -d' ' -f1)
    [ "$s" = "$SUM" ] && echo "md5 OK" || echo "md5 MISMATCH"
}

summary=$RES/summary.txt
echo "file: test.dat $SIZE MiB ($((SIZE * 1048576)) bytes), network: tc2 (bandwidth only)" > $summary

run() {   # $1=label $2... = node1 で実行するコマンド
    local label=$1; shift
    echo "=== $label: $*" | tee $RES/$label.log
    local t0=$(date +%s.%N)
    ip netns exec node1 "$@" >> $RES/$label.log 2>&1
    local t1=$(date +%s.%N)
    local wall=$(echo "$t1 - $t0" | bc)
    local mbps=$(echo "scale=2; $SIZE * 1048576 * 8 / $wall / 1000000" | bc)
    local ok=$(check "$label" received.dat)
    printf "%-34s wall %7.2f s  %7.2f Mbps  %s\n" "$label" "$wall" "$mbps" "$ok" | tee -a $summary
    rm -f received.dat
}

stop_all
# A
ip netns exec node3 $B43/fserver43.out test.dat 10000 > logs/fserver43.log 2>&1 &
sleep 0.5
run A_baseline_direct $B43/fclient43.out 172.21.0.30 10000 received.dat
stop_all

# B
ip netns exec node3 $B43/fserver43.out test.dat 10000 > logs/fserver43.log 2>&1 &
python3 routeplan.py --path 3-4-5-1 > $RES/plan_B.txt
start_plan
run B_route_only_3-4-5-1 $B43/fclient43.out 172.23.0.50 20001 received.dat
stop_all

# C
python3 routeplan.py --path 3-1 > $RES/plan_C.txt
start_plan
run C_program_only_direct ./fclient.out -o received.dat 172.21.0.30:10000
stop_all

# D
python3 routeplan.py --mode single > $RES/plan_D.txt
start_plan
run D_route+program_3-4-5-1 ./fclient.out -o received.dat 172.23.0.50:20001
stop_all

# E
python3 routeplan.py --mode multi > $RES/plan_E.txt
start_plan
cmd=$(grep -o '\./fclient.out .*' plan/node1.sh)
run E_multipath_maxflow $cmd
stop_all

# E2: 1 Mbps の経路も含めた場合（含めない理由の確認）
python3 routeplan.py --mode multi --min-mbps 0 > $RES/plan_E2.txt
start_plan
cmd=$(grep -o '\./fclient.out .*' plan/node1.sh)
run E2_multipath_incl_1Mbps $cmd
stop_all

rm -rf $B43
echo; cat $summary
