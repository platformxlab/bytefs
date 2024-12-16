#!/bin/bash
target_mem_mb=$1

utils_dir=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
ctrl_program="$utils_dir/ctrl"

if ! [[ "$target_mem_mb" =~ ^[0-9]+\.?[0-9]*$ ]] ; then
  printf "Target mem not a number: %s\n" "$target_mem_mb" >&2
  exit 1
fi

pin_required=$(echo "($(grep MemAvailable /proc/meminfo | sed -rn "s/MemAvailable:\s*([0-9]*)\s*kB/\1/p")-$target_mem_mb*1024)/1024" | bc)
if [ "$pin_required" -le 0 ]; then
  echo "Error try pinning negative mem $pin_required MB" >&2
  exit 0
fi
echo "$pin_required"
sudo "$ctrl_program" 906 "$pin_required"
