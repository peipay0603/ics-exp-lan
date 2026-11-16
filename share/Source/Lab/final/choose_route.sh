#!/bin/bash
set -eu

if [ "$#" -lt 6 ]; then
    echo "Usage: $0 probe_port transfer_port probe_file output_file result_dir host..." >&2
    exit 1
fi
probe_port=$1
transfer_port=$2
probe_file=$3
output_file=$4
result_dir=$5
shift 5
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$result_dir"
printf 'host\tseconds\tgoodput_mbps\n' > "$result_dir/routes.tsv"
best_host=
best_time=

for host in "$@"; do
    if timeout 60 "$script_dir/fclient.out" "$host" "$probe_port" "$result_dir/probe-$host.dat" > "$result_dir/probe-$host.log" 2>&1; then
        if ! cmp -s "$probe_file" "$result_dir/probe-$host.dat"; then
            echo "probe mismatch: $host" >&2
            continue
        fi
        seconds=$(awk '/^Time:/ {print $2}' "$result_dir/probe-$host.log")
        mbps=$(awk '/^Goodput:/ {gsub(/[()]/, "", $4); print $4}' "$result_dir/probe-$host.log")
        printf '%s\t%s\t%s\n' "$host" "$seconds" "$mbps" >> "$result_dir/routes.tsv"
        if [ -z "$best_host" ] || awk -v value="$seconds" -v best="$best_time" 'BEGIN {exit !(value < best)}'; then
            best_host=$host
            best_time=$seconds
        fi
    fi
done

if [ -z "$best_host" ]; then
    echo 'no working route' >&2
    exit 1
fi
echo "$best_host" > "$result_dir/selected-host.txt"
echo "Selected route: $best_host ($best_time sec for probe)"
time -p "$script_dir/fclient.out" "$best_host" "$transfer_port" "$output_file" > "$result_dir/transfer.log" 2> "$result_dir/transfer.err"
