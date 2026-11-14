#!/bin/bash
# exp6/bench.sh : node3 → node1 の転送を繰り返し測り、results/bench.csv に1行ずつ追記する
#
# ホストPC（docker コマンドが使える端末）で実行する。コンテナの中ではない。
# 中継（チームの relay や udp_relay.out）は先に node4・node5 で起動しておくこと。
#
#   ./bench.sh tcp DEST  BUF_LEN [BUF_LEN ...]   … tcp_send/tcp_recv（-b を両方に指定）
#   ./bench.sh hyb DESTS CHUNK   [CHUNK ...]     … hyb_send/hyb_recv（-c を指定）
#
#   DEST  : tcp_send の接続先 IP[:PORT]（直接なら 172.21.0.10、中継なら node4 の 172.27.0.40）
#   DESTS : hyb_send の UDP 宛先 "IP:PORT@Mbps ..."（複数なら空白区切りで1つの引数に）
#
# 例:
#   ./bench.sh tcp 172.21.0.10 1000 64K                  # 条件A・C（直接 3→1）
#   ./bench.sh tcp 172.27.0.40 1000 64K                  # 条件B・D（3→4→5→1、relay起動済み）
#   ./bench.sh hyb 172.27.0.40:20000@50 1464             # UDP+TCP（3→4→5→1、udp_relay起動済み）
#
# 環境変数（省略可）:
#   FILE=test100M.dat  送るファイル（exp6 フォルダ内。make data SIZE=100 で作る）
#   REPEAT=3           各条件の繰り返し回数
#   PORT=20000         node1 の受信ポート（tcp の DEST に :PORT が無ければこれに接続）
#   CTRL=172.21.0.10   hyb の制御用TCPの接続先（node1。既定は直接回線 3→1）
#   SEND_OPTS / RECV_OPTS  送信・受信プログラムに追加するオプション
#   LABEL=メモ         CSV の label 列に入れる文字列
#   TIMEOUT=900        1回の転送の制限時間 [秒]

set -u
cd "$(dirname "$0")"

DIR=/home/ubuntu/share/Source/Lab/exp6
FILE=${FILE:-test100M.dat}
REPEAT=${REPEAT:-3}
PORT=${PORT:-20000}
CTRL=${CTRL:-172.21.0.10}
SEND_OPTS=${SEND_OPTS:-}
RECV_OPTS=${RECV_OPTS:-}
LABEL=${LABEL:-}
TIMEOUT=${TIMEOUT:-900}
OUT=/tmp/exp6_recv.dat
CSV=results/bench.csv

if [ $# -lt 3 ] || { [ "$1" != tcp ] && [ "$1" != hyb ]; }; then
    sed -n '2,30p' "$0"
    exit 1
fi
MODE=$1
DEST=$2
shift 2

if [ ! -f "$FILE" ]; then
    echo "$FILE がありません。node3 などで make data SIZE=100 を実行してください" >&2
    exit 1
fi
for p in tcp_send.out tcp_recv.out hyb_send.out hyb_recv.out; do
    if [ ! -x "$p" ]; then
        echo "$p がありません。コンテナ内で make を実行してください" >&2
        exit 1
    fi
done

mkdir -p results
if [ ! -f "$CSV" ]; then
    echo "datetime,mode,dest,param,bytes,run,time_s,goodput_mbps,send_cpu_s,recv_cpu_s,rounds,retrans,cmp,label" > "$CSV"
fi

TMPS=$(mktemp)
TMPR=$(mktemp)
trap 'rm -f "$TMPS" "$TMPR"' EXIT

# RESULT 行から key=value の値を取り出す
kv() {
    grep '^RESULT' "$1" | tr ' ' '\n' | grep "^$2=" | head -1 | cut -d= -f2
}

case "$DEST" in
    *:*) TCP_DEST_IP=${DEST%%:*}; TCP_DEST_PORT=${DEST##*:} ;;
    *)   TCP_DEST_IP=$DEST;       TCP_DEST_PORT=$PORT ;;
esac

FIRST_ROW=$(($(wc -l < "$CSV") + 1))

for PARAM in "$@"; do
    for RUN in $(seq 1 "$REPEAT"); do
        if [ "$MODE" = tcp ]; then
            RECV_CMD="./tcp_recv.out -b $PARAM $RECV_OPTS $PORT $OUT"
            SEND_CMD="./tcp_send.out -b $PARAM $SEND_OPTS $TCP_DEST_IP $TCP_DEST_PORT $FILE"
        else
            RECV_CMD="./hyb_recv.out $RECV_OPTS $PORT $OUT"
            SEND_CMD="./hyb_send.out -c $PARAM $SEND_OPTS $CTRL $PORT $FILE $DEST"
        fi

        # 前回の受信プログラムが残っていたら止める
        docker exec node1 pkill -f 'tcp_recv.out|hyb_recv.out' >/dev/null 2>&1
        docker exec node1 bash -c "cd $DIR && exec timeout $TIMEOUT $RECV_CMD" > "$TMPR" 2>&1 &
        RPID=$!
        sleep 0.5
        docker exec node3 bash -c "cd $DIR && exec timeout $TIMEOUT $SEND_CMD" > "$TMPS" 2>&1
        wait $RPID

        if docker exec node1 cmp -s "$DIR/$FILE" "$OUT"; then CMP=OK; else CMP=NG; fi
        if [ "$MODE" = tcp ]; then
            ROUNDS=""; RETRANS=""
        else
            ROUNDS=$(kv "$TMPS" rounds); RETRANS=$(kv "$TMPS" retrans)
        fi
        TIME=$(kv "$TMPR" time)
        GOODPUT=$(kv "$TMPR" goodput_mbps)
        BYTES=$(kv "$TMPR" bytes)
        SCPU=$(kv "$TMPS" cpu)
        RCPU=$(kv "$TMPR" cpu)

        echo "$(date '+%Y-%m-%d %H:%M:%S'),$MODE,\"$DEST\",$PARAM,$BYTES,$RUN,$TIME,$GOODPUT,$SCPU,$RCPU,$ROUNDS,$RETRANS,$CMP,$LABEL" >> "$CSV"
        printf '%s %-24s param=%-6s run=%d  time=%ss  goodput=%sMbps  cpu(send/recv)=%s/%s  cmp=%s\n' \
            "$MODE" "$DEST" "$PARAM" "$RUN" "${TIME:-?}" "${GOODPUT:-?}" "${SCPU:-?}" "${RCPU:-?}" "$CMP"
        if [ -z "$GOODPUT" ] || [ "$CMP" != OK ]; then
            echo "---- sender ----";   cat "$TMPS"
            echo "---- receiver ----"; cat "$TMPR"
        fi
    done
done

# 今回追記した行について、条件ごとの平均を表示する
echo "== average (this run) =="
tail -n +"$FIRST_ROW" "$CSV" | awk -F, '
    $13 == "OK" { key = $2 " " $3 " param=" $4
                  if (!(key in n)) order[++m] = key
                  n[key]++; g[key] += $8; t[key] += $7; c[key] += $9 }
    END { for (i = 1; i <= m; i++) { k = order[i]
              printf "%s  n=%d  goodput=%.3f Mbps  time=%.3f s  send_cpu=%.3f s\n",
                     k, n[k], g[k] / n[k], t[k] / n[k], c[k] / n[k] } }'
