#!/bin/bash
set -euo pipefail
plan_dir=$(cd -- "$(dirname -- "$0")" && pwd)
cd -- "$plan_dir"/../../../..
time -p ./fclient.out -o /home/ubuntu/share/route/results/20261116-final900/B-01/received-a3d3d45f6fbc46b9a35be7e9ddadcae2.dat -c 256 -d 4 -m 5.0 172.23.0.50:31101@50=3-4-5-1 "$@"
