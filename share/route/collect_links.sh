#!/bin/bash
# collect_links.sh -- 自ノードから出るリンクの帯域と遅延を調べて links/nodeN.txt に書く
#
#   各ノード（node1〜node5）で 1 回ずつ実行する:
#     $ ./collect_links.sh
#
# 調べ方:
#   - ip -o -4 addr で 172.2X.0.N0 形式のアドレスを探し，自分のノード番号 N と
#     各インタフェースがどのノードとつながっているか（サブネット X）を判定する
#   - tc qdisc show で送信側に設定されている tbf（帯域）と netem（遅延）を読む
#     （tbf が無いリンクは帯域制限なしとして 1000 Mbps と書く）
# 出力（1 行 1 リンク，送信方向）:  自ノード 相手ノード 帯域Mbps 遅延ms インタフェース
#
# share は全ノードで共有されているので，全ノードで実行すれば links/ に全リンクが揃う．
set -eu
cd "$(dirname "$0")"
mkdir -p links

# サブネット 172.2X → つながっている 2 ノード（docker-compose.yml の lanA-B）
pair_of() {
    case "$1" in
        20) echo "1 2" ;; 21) echo "1 3" ;; 22) echo "1 4" ;; 23) echo "1 5" ;;
        24) echo "2 3" ;; 25) echo "2 4" ;; 26) echo "2 5" ;;
        27) echo "3 4" ;; 28) echo "3 5" ;; 29) echo "4 5" ;;
        *) echo "" ;;
    esac
}

# tc の rate 表記（50Mbit, 500Kbit, 1Gbit, 800bit）を Mbps に変換
to_mbps() {
    echo "$1" | awk '{
        v = $1 + 0; u = $1; sub(/^[0-9.]+/, "", u);
        if (u ~ /^[Gg]/) v *= 1000; else if (u ~ /^[Kk]/) v /= 1000;
        else if (u ~ /^[Mm]/) v *= 1; else v /= 1000000;
        printf "%g", v }'
}

# netem の delay 表記（10ms, 2.5ms, 250us, 1s）を ms に変換
to_ms() {
    echo "$1" | awk '{
        v = $1 + 0; u = $1; sub(/^[0-9.]+/, "", u);
        if (u == "us") v /= 1000; else if (u == "s") v *= 1000;
        printf "%g", v }'
}

me=""
lines=""
while read -r ifname addr; do
    ip4=${addr%/*}
    case "$ip4" in 172.2[0-9].0.[1-5]0) ;; *) continue ;; esac
    sub=$(echo "$ip4" | cut -d. -f2 | cut -c2-)      # 172.2X → X
    sub="2$sub"
    host=$(echo "$ip4" | cut -d. -f4)                # N0
    n=$((host / 10))
    me=$n
    set -- $(pair_of "$sub")
    [ $# -eq 2 ] || continue
    if [ "$1" -eq "$n" ]; then peer=$2; else peer=$1; fi

    q=$(tc qdisc show dev "$ifname" 2>/dev/null || true)
    rate=$(echo "$q" | grep -o 'tbf .*rate [0-9.]*[A-Za-z]*' | awk '{print $NF}' | head -1)
    delay=$(echo "$q" | grep -o 'netem .*delay [0-9.]*[a-z]*' | awk '{print $NF}' | head -1)
    mbps=1000
    ms=0
    [ -n "$rate" ] && mbps=$(to_mbps "$rate")
    [ -n "$delay" ] && ms=$(to_ms "$delay")
    lines="${lines}${n} ${peer} ${mbps} ${ms} ${ifname}
"
done < <(ip -o -4 addr show | awk '{print $2, $4}')

if [ -z "$me" ]; then
    echo "172.2X.0.N0 形式のアドレスが見つかりません（実験用ノードで実行してください）" >&2
    exit 1
fi

out="links/node${me}.txt"
{
    echo "# src dst Mbps delay_ms iface   (collected on node${me} $(date '+%F %T'))"
    printf "%s" "$lines" | sort -n -k2
} > "$out"
echo "wrote $out"
cat "$out"
