#!/bin/bash
# sim_mesh.sh -- Docker なしで実験ネットワークを再現する（Linux の network namespace）
#
#   sudo ./sim_mesh.sh up [1-4]   … node1〜node5 の namespace とフルメッシュを作り，
#                                   リポジトリの Makefile の tcN の設定を適用する
#   sudo ./sim_mesh.sh apply N    … 作ったメッシュに tcN の設定を上書きする（転送中の設定変更の再現）
#   sudo ./sim_mesh.sh down       … 片付け
#   sudo ip netns exec node3 <コマンド>   … node3 の中でコマンドを実行
#
# インタフェース名（eth0〜eth3）と IP は docker-compose.yml / fix_eth_order.sh と同じ．
# netem（遅延）が使えないカーネルでは遅延なし・帯域制限（tbf）だけで再現する．
set -eu
cd "$(dirname "$0")"
MAKEFILE=../../../Makefile

# リンク: サブネット 2 ノード
LINKS="20:1:2 21:1:3 22:1:4 23:1:5 24:2:3 25:2:4 26:2:5 27:3:4 28:3:5 29:4:5"

# ノード n の ethK はつながっているサブネットを小さい順に並べた K 番目
eth_of() {   # $1=node $2=subnet
    local k=0 l s a b
    for l in $LINKS; do
        IFS=: read -r s a b <<< "$l"
        if [ "$a" = "$1" ] || [ "$b" = "$1" ]; then
            [ "$s" = "$2" ] && { echo "eth$k"; return; }
            k=$((k + 1))
        fi
    done
}

up() {
    local tcn=${1:-2} l s a b i
    down 2>/dev/null || true
    for i in 1 2 3 4 5; do
        ip netns add node$i
        ip -n node$i link set lo up
    done
    for l in $LINKS; do
        IFS=: read -r s a b <<< "$l"
        ip link add "v${s}a" type veth peer name "v${s}b"
        ip link set "v${s}a" netns node$a
        ip link set "v${s}b" netns node$b
        ip -n node$a link set "v${s}a" name "$(eth_of $a $s)"
        ip -n node$b link set "v${s}b" name "$(eth_of $b $s)"
        ip -n node$a addr add 172.$s.0.${a}0/16 dev "$(eth_of $a $s)"
        ip -n node$b addr add 172.$s.0.${b}0/16 dev "$(eth_of $b $s)"
        ip -n node$a link set "$(eth_of $a $s)" up
        ip -n node$b link set "$(eth_of $b $s)" up
    done
    apply "$tcn"
}

apply() {
    local tcn=${1:-2}
    # Makefile の tcN の "docker exec -u 0 nodeX tc ..." を namespace で実行する
    local applied=0 skipped=0
    while read -r node cmd; do
        # 転送中に tbf を置き換えると，キューに残った大きな（GSO でまとめられた）パケットが
        # 新しい burst より大きいときに永久に送れなくなる．いったん消してから作り直す
        case "$cmd" in
            *"root handle 1: tbf"*)
                dev=$(echo "$cmd" | awk '{for (i = 1; i < NF; i++) if ($i == "dev") print $(i + 1)}')
                ip netns exec "$node" tc qdisc del dev "$dev" root 2>/dev/null || true ;;
        esac
        if ip netns exec "$node" $cmd 2>/dev/null; then
            applied=$((applied + 1))
        else
            skipped=$((skipped + 1))
        fi
    done < <(awk -v t="tc$tcn:" '
        $0 == t {on = 1; next}
        on && /^[^ \t]/ {on = 0}
        on && /docker exec -u 0 node[0-9] tc / {
            sub(/.*docker exec -u 0 /, ""); print }' "$MAKEFILE")
    echo "network tc$tcn: applied $applied tc commands, skipped $skipped (netem delay is unsupported here)"
}

down() {
    for i in 1 2 3 4 5; do ip netns del node$i 2>/dev/null || true; done
}

case "${1:-}" in
    up)    up "${2:-2}" ;;
    apply) apply "${2:-2}" ;;
    down) down ;;
    *)     echo "Usage: sudo $0 up [1-4] | apply [1-4] | down" >&2; exit 1 ;;
esac
