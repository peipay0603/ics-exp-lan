#!/bin/bash
set -euo pipefail
plan_dir=$(cd -- "$(dirname -- "$0")" && pwd)
cd -- "$plan_dir"/../../../..
time -p ./fclient.out -o /home/ubuntu/share/route/results/20261116-smoke/C-01/received-66ba89fe2efd41a1ae7caf07b005977f.dat -c 256 -d 4 -m 5.0 172.21.0.30:31000@10=3-1
