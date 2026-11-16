#!/bin/bash
set -euo pipefail
plan_dir=$(cd -- "$(dirname -- "$0")" && pwd)
cd -- "$plan_dir"/../../../..
time -p ./fclient.out -o /home/ubuntu/share/route/results/20261116-final900/A-01/received-3cba2ae944614d0b984423a625f5e817.dat -c 256 -d 4 -m 5.0 172.21.0.30:31000@10=3-1
