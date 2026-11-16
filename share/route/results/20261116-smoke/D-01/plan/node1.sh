#!/bin/bash
set -euo pipefail
plan_dir=$(cd -- "$(dirname -- "$0")" && pwd)
cd -- "$plan_dir"/../../../..
time -p ./fclient.out -o /home/ubuntu/share/route/results/20261116-smoke/D-01/received-cdb639fcc0d44d7a8729f1f0430650d5.dat -c 256 -d 4 -m 5.0 172.23.0.50:31101@50=3-4-5-1
