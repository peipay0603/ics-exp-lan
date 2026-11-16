#!/usr/bin/env bash
# 既存の呼出し方を維持。例: ./bench.sh tcp 172.21.0.10 1000 64K
# 比較条件A〜EとUDP併用: FILE=test100M.dat REPEAT=3 ./bench.sh suite
set -eu
cd "$(dirname "$0")"
exec python3 bench.py "$@"
