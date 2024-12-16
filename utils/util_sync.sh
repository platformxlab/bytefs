#!/bin/bash

vm_target=""
while getopts "t:" arg; do
  case $arg in
    t)
      vm_target=${OPTARG}
      ;;
    *)
      exit 1
      ;;
  esac
done

source bash_utils.sh

[ -z "$vm_target" ]
assert_exit $? "\n"

target_files=(
  bash_utils.sh
  pin_mem.sh
  filebench_run.sh
  rocksdb_option.ini
  filebench_workloads
  ycsb_workloads
)

for target_file in "${target_files[@]}"; do
  set -x
  scp -r "$target_file" "$vm_target:/home/csl/Byte_fs/utils"
  set +x
done
