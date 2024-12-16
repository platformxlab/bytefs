#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
ENDC='\033[0m'

utils_dir=/home/csl/Byte_fs/utils
scripts_dir="$utils_dir/scripts"
post_dir="$utils_dir/benchbase/target/benchbase-postgres"

workloads=(tpcc tatp twitter seats)
ctrl_program="$utils_dir/ctrl"
post_workloads_dir="$post_dir/config/postgres"
output_folder=output
mkdir -p "$output_folder"

"$ctrl_program" 41

for workload in "${workloads[@]}"; do
  output_log_name="$workload"
  printf "${CYAN}Operating on %s warmup${ENDC}\n" "$workload"
  java -jar $post_dir/benchbase.jar -b $workload -c $post_workloads_dir/sample_${workload}_config.xml --create=true --load=true
  printf "${CYAN}Operating on %s run${ENDC}\n" "$workload"
  "$ctrl_program" 40
  java -jar $post_dir/benchbase.jar -b $workload -c $post_workloads_dir/sample_${workload}_config.xml --execute=true
  "$ctrl_program" 42 > "$output_folder/$output_log_name.info" 2>&1
  "$ctrl_program" 41
  "$ctrl_program" 0
done