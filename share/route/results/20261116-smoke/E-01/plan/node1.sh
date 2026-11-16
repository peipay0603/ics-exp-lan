#!/bin/bash
set -euo pipefail
plan_dir=$(cd -- "$(dirname -- "$0")" && pwd)
cd -- "$plan_dir"/../../../..
time -p ./fclient.out -o /home/ubuntu/share/route/results/20261116-smoke/E-01/received-e3c7ba063bfa49faa5e695ef22bc4219.dat -c 256 -d 4 -m 5.0 172.23.0.50:31101@40=3-4-5-1 172.21.0.30:31000@10=3-1 172.22.0.40:31103@10=3-4-1 172.23.0.50:31104@9=3-2-5-1
