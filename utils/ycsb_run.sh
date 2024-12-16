#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
ENDC='\033[0m'

utils_dir=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
scripts_dir="$utils_dir/scripts"
ycsb_dir="$HOME/YCSB"

workloads=( workloada workloadb workloadc workloadd workloade workloadf )
workloads=( workloadd workloade workloadf )
workloads=( workloadf )
data_dir=/mnt/pmem0/data/ycsb-rocksdb-data
ctrl_program="$utils_dir/ctrl"
ycsb_program="$ycsb_dir/bin/ycsb"
ycsb_workloads_dir="$utils_dir/ycsb_workloads"
output_folder=output
num_threads=32
repetition=2

sudo chown -R "$(whoami)" /mnt/pmem0
mkdir -p "$output_folder"
printf "${RED}Loading dataset${ENDC}\n"
"$ycsb_program" load rocksdb -s -P "$ycsb_workloads_dir/$workload" -p rocksdb.dir="$data_dir" -threads "$num_threads" > "$output_folder/load.log" 2>&1

"$ctrl_program" 41
for workload in "${workloads[@]}"; do
  output_log_name="$workload"
  for iter in $(seq 1 $repetition); do
    printf "${CYAN}Operating on %s @ iter %d${ENDC}\n" "$workload" "$iter"
    "$ycsb_program" run rocksdb -s -P "$ycsb_workloads_dir/$workload" -p rocksdb.dir="$data_dir" -threads "$num_threads" > "$output_folder/$output_log_name.iter$iter.log" 2>&1
  done
  "$ctrl_program" 40
  "$ycsb_program" run rocksdb -s -P "$ycsb_workloads_dir/$workload" -p rocksdb.dir="$data_dir" -threads "$num_threads" > "$output_folder/$output_log_name.final.log" 2>&1
  "$ctrl_program" 42 > "$output_folder/$output_log_name.info" 2>&1
  "$ctrl_program" 41
  "$ctrl_program" 0
done
