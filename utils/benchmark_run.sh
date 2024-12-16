#!/bin/bash

# Folder absolute paths {{{
utils_dir=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
base_dir=$(dirname -- "$utils_dir")
filebench_workloads_dir="$utils_dir/filebench_workloads"
ycsb_workloads_dir="$utils_dir/ycsb_workloads"
filebench_bin_dir="$utils_dir/filebench"
ycsb_bin_dir="$utils_dir/YCSB"
flamegraph_dir="$utils_dir/FlameGraph"
output_folder="$utils_dir/output"
source "$utils_dir/bash_utils.sh"

mountpoint="/mnt/edrive"
# }}}

# Setting variables {{{
workload=""
workload_warm=""
filesystem=""
output_folder_tag=""
perf=0
dry_run=0
perf_subprocess_id=0

ycsb_data_dir="$mountpoint/ycsb-rocksdb-data"
ycsb_num_threads=16
# }}}

# Local functions {{{
drop_cache() {
  if [ "$1" -eq 0  ]; then
    printf "Skip drop cache <%d>\n" "$1" 2>&1
    return 0
  fi    
  printf "Drop cache <%d>\n" "$1" 2>&1
  sync; echo "$1" | sudo tee /proc/sys/vm/drop_caches > /dev/null
}
# }}}

# Parse command line args {{{
while getopts "w:f:t:pd" arg; do
  case $arg in
    w)
      workload=${OPTARG}
      ;;
    f)
      filesystem=${OPTARG}
      ;;
    t)
      output_folder_tag=${OPTARG}
      if ! [[ "$output_folder_tag" =~ ^[0-9a-zA-Z\-]*$ ]]; then
        printf "Tag must match regex ^[0-9a-zA-Z\-]*\$, which <%s> does not\n" "$output_folder_tag"
        exit 1
      fi
      ;;
    p)
      perf=1
      ;;
    d)
      dry_run=1
      ;;
    *)
      exit 1
      ;;
  esac
done
# }}}

# Workload and filesystem settting {{{
declare -A filebench_workload_warm_mapping
filebench_workload_warm_mapping=(
  ["create"]=""
  ["delete"]="delete_warm"
  ["mkdir"]=""
  ["rmdir"]="rmdir_warm"
  ["stat"]="stat_warm"
  ["fileserver"]="fileserver_warm"
  ["oltp"]="oltp_warm"
  ["varmail"]="varmail_warm"
  ["webproxy"]="webproxy_warm"
  ["webserver"]="webserver_warm"
)
declare -A ycsb_workload_warm_mapping
ycsb_workload_warm_mapping=(
  ["workloada"]="workloada"
  ["workloadb"]="workloadb"
  ["workloadc"]="workloadc"
  ["workloadd"]="workloadd"
  ["workloade"]="workloade"
  ["workloadf"]="workloadf"
)
declare -A workload_warm_drop_cache
workload_warm_drop_cache=(
  # filebench
  ["delete"]=1
  ["rmdir"]=1
  ["stat"]=1
  ["fileserver"]=0
  ["oltp"]=0
  ["varmail"]=0
  ["webproxy"]=0
  ["webserver"]=0
  # ycsb
  ["workloada"]=0
  ["workloadb"]=0
  ["workloadc"]=0
  ["workloadd"]=0
  ["workloade"]=0
  ["workloadf"]=0
)
declare -A workload_warm_run_again
workload_warm_run_again=(
  # filebench
  ["delete"]=0
  ["rmdir"]=0
  ["stat"]=1
  ["fileserver"]=1
  ["oltp"]=1
  ["varmail"]=1
  ["webproxy"]=1
  ["webserver"]=1
  # ycsb
  ["workloada"]=1
  ["workloadb"]=1
  ["workloadc"]=1
  ["workloadd"]=1
  ["workloade"]=1
  ["workloadf"]=1
)

filesystems=( ext4 f2fs nova pmfs bytefs )
# }}}

# Sanity check and set warmup config {{{
# workload senity check
workload_warm=""
workload_bench=""
if [[ -f "$filebench_workloads_dir/$workload.f" ]]; then
  workload_bench=filebench
  ! [[ -v "filebench_workload_warm_mapping[$workload]" ]]
  assert_exit $? "Workload <filebench/$workload.f> warmup config mapping not found\n" 2>&1
  workload_warm="${filebench_workload_warm_mapping[$workload]}"

  [[ -n $workload_warm ]] && ! [[ -f "$filebench_workloads_dir/$workload_warm.f" ]]
  assert_exit $? "Workload warmup <$workload_warm> does not exist\n" 2>&1
elif [[ -f "$ycsb_workloads_dir/$workload" ]]; then
  workload_bench=ycsb
  ! [[ -v "ycsb_workload_warm_mapping[$workload]" ]]
  assert_exit $? "Workload <ycsb/$workload> warmup config mapping not found\n" 2>&1
  workload_warm="${ycsb_workload_warm_mapping[$workload]}"

  [[ -n $workload_warm ]] && ! [[ -f "$ycsb_workloads_dir/$workload_warm" ]]
  assert_exit $? "Workload warmup <$workload_warm> does not exist\n" 2>&1

  export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64
else
  assert_exit 0 "Workload <$workload> does not exist\n" 2>&1
fi

output_log_name="$workload"
warmup_drop_cache=0
warmup_run_again=0
if [[ -n $workload_warm ]]; then
  ! [[ -v "workload_warm_drop_cache[$workload]" ]]
  assert_exit $? "Workload <$workload> warmup drop cache config not found\n" 2>&1
  warmup_drop_cache="${workload_warm_drop_cache[$workload]}"
  ! [[ -v "workload_warm_run_again[$workload]" ]]
  assert_exit $? "Workload <$workload> warmup run again config not found\n" 2>&1
  warmup_run_again="${workload_warm_run_again[$workload]}"
fi

if [ "$workload_bench" == "filebench" ]; then
  workload="$workload.f"
  if [ -n "$workload_warm" ]; then workload_warm="$workload_warm.f"; fi
fi

if [ -z "$workload_warm" ]; then
  workload_warm="<no warmup>"
fi

# filesystem senity check
if ! [[ " ${filesystems[*]} " =~ " ${filesystem} " ]]; then
  printf "Filesystem <%s> does not exist\n" "$filesystem" 2>&1
  exit 1
fi

printf "Running workload <%s> with warmup <%s>\n" "$workload" "$workload_warm" 2>&1

# output folder senity check
if [[ -n "$output_folder_tag" ]]; then
  output_folder="$output_folder-$output_folder_tag"
else
  output_folder_tag="<no tag>"
fi

perf_postfix=""
if [ "$perf" -ne 0 ]; then
  perf_postfix=".perf"
fi
stat_output_file_prefix="$filesystem.$output_log_name$perf_postfix"

if [[ -d "$output_folder" ]]; then
  printf "Overwriting output file %s\n" "$output_folder/$stat_output_file_prefix"
fi

if [ "$filesystem" != "f2fs" ] && [ "$workload_bench" != "ycsb" ]; then
  drop_cache_options=3
else
  drop_cache_options=0
fi

# Debug {{{
printf "workload:                 %s\n" "$workload" 2>&1
printf "workload_warm:            %s\n" "$workload_warm" 2>&1
printf "workload_warm_drop_cache: %s\n" "$warmup_drop_cache" 2>&1
printf "filesystem:               %s\n" "$filesystem" 2>&1
printf "perf:                     %s\n" "$perf" 2>&1
printf "output_tag:               %s\n" "$output_folder_tag" 2>&1
printf "output_folder:            %s\n" "$output_folder" 2>&1
printf "dry_run:                  %s\n" "$dry_run" 2>&1
printf "stat_output_file_prefix:  %s\n" "$stat_output_file_prefix" 2>&1
# exit 0
# }}}

# }}}

# Program variables {{{
ctrl_program="$utils_dir/ctrl"
filebench_program="$filebench_bin_dir/filebench"
ycsb_program="$ycsb_bin_dir/bin/ycsb"
flamegraph_stack_collapse_program="$flamegraph_dir/stackcollapse-perf.pl"
flamegraph_analyze_program="$flamegraph_dir/flamegraph.pl"
perf_program="$base_dir/linux/tools/perf/perf"
pin_mem_program="$utils_dir/pin_mem.sh"
# }}}

# Program variables sanity check {{{
if ! [[ -f "$filebench_program" ]]; then
  assert_exit 0 "Filebench program %s does not exist" "$filebench_program"
fi
if ! [[ -f "$ycsb_program" ]]; then
  assert_exit 0 "YCSB program %s does not exist" "$ycsb_program"
fi
if [[ "$perf" -ne 0 ]]; then
  if ! [[ -f "$flamegraph_stack_collapse_program" ]]; then
    assert_exit 0 "Framegraph program %s does not exist" "$flamegraph_stack_collapse_program"
  fi
  if ! [[ -f "$flamegraph_analyze_program" ]]; then
    assert_exit 0 "Framegraph program %s does not exist" "$flamegraph_analyze_program"
  fi
  if ! [[ -f "$perf_program" ]]; then
    assert_exit 0 "Perf program %s does not exist" "$perf_program"
  fi
fi
# }}}

# Dry run termination {{{
if [ "$dry_run" -ne 0 ]; then
  exit 0
fi
# }}}

# Workspace setup {{{
sudo chown -R "$(whoami)" "$mountpoint"
mkdir -p "$output_folder"
make -C "$utils_dir" clean >&2 && make -C "$utils_dir" >&2
# }}}

# General preparation {{{
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space > /dev/null
"$ctrl_program" 41

printf "Mounting\n" 1>&2
"$utils_dir/set${filesystem}.sh" > /dev/null
drop_cache "$drop_cache_options"
# }}}

# Warmup {{{
if [ -n "$workload_warm" ]; then
  printf "Operating on warmup %s, start to run #1\n" "$workload_warm" 1>&2
  if [[ "$workload_bench" == "filebench" ]]; then
    sudo "$filebench_program" -f "$filebench_workloads_dir/$workload_warm" > /dev/null 2>&1
  elif [[ "$workload_bench" == "ycsb" ]]; then
    pushd "$ycsb_bin_dir" > /dev/null 2>&1
    printf "####### LOAD START\n"
    "$ycsb_program" load rocksdb -s -P "$ycsb_workloads_dir/$workload" -p rocksdb.dir="$ycsb_data_dir" -p rocksdb.optionsfile="$utils_dir/rocksdb_option.ini" -threads "$ycsb_num_threads" 2>&1 | sed -e "s/^/### /" >&2
    printf "####### LOAD END\n"
    popd > /dev/null 2>&1
  fi

  drop_cache "$drop_cache_options"
  "$pin_mem_program" 4096

  if [ "$warmup_run_again" -ne 0 ]; then
    printf "Operating on warmup %s, start to run #2\n" "$workload_warm" 1>&2
    if [[ "$workload_bench" == "filebench" ]]; then
      sudo "$filebench_program" -f "$filebench_workloads_dir/$workload_warm" > /dev/null 2>&1
    elif [[ "$workload_bench" == "ycsb" ]]; then
      pushd "$ycsb_bin_dir" > /dev/null 2>&1
      "$ycsb_program" run rocksdb -s -P "$ycsb_workloads_dir/$workload_warm" -p rocksdb.dir="$ycsb_data_dir" -p rocksdb.optionsfile="$utils_dir/rocksdb_option.ini" -threads "$ycsb_num_threads" > /dev/null 2>&1
      popd > /dev/null 2>&1
    fi
  fi

  if [ "$warmup_drop_cache" -ne 0 ]; then
    drop_cache "$drop_cache_options"
  fi
fi

sleep 10
printf "Operating on %s, start to run\n" "$workload" 1>&2
# }}}

# Main program {{{
printf "Run started\n"
cat /dev/null > "$output_folder/$stat_output_file_prefix.log"
"$ctrl_program" 40

# Perf start {{{
if [ "$perf" != 0 ]; then
  echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid > /dev/null
  echo 0 | sudo tee /proc/sys/kernel/kptr_restrict > /dev/null
  printf "!!! Perf start\n" 1>&2
  "$perf_program" record -F 99 -a -g &
  perf_subprocess_id=$!
  printf "!!! Perf started @ pid <%d>\n" "$perf_subprocess_id" 1>&2
fi
# }}}

if [[ "$workload_bench" == "filebench" ]]; then
  sudo "$filebench_program" -f "$filebench_workloads_dir/$workload" 2>&1 | tee -a "$output_folder/$stat_output_file_prefix.log"
elif [[ "$workload_bench" == "ycsb" ]]; then
  pushd "$ycsb_bin_dir" > /dev/null 2>&1
  "$ycsb_program" run rocksdb -s -P "$ycsb_workloads_dir/$workload" -p rocksdb.dir="$ycsb_data_dir" -p rocksdb.optionsfile="$utils_dir/rocksdb_option.ini" -threads "$ycsb_num_threads" 2>&1 | tee -a "$output_folder/$stat_output_file_prefix.log"
  popd > /dev/null 2>&1
fi

# Perf end {{{
if [ "$perf" != 0 ]; then
  printf "!!! Terminating perf @ pid <%d> ...\n" "$perf_subprocess_id" 1>&2
  kill -INT "$perf_subprocess_id"
  wait "$perf_subprocess_id"
  printf "!!! Termination done\n" 1>&2
fi
# }}}

"$ctrl_program" 42 2>&1 | tee -a "$output_folder/$stat_output_file_prefix.log"
# cat "$output_folder/$stat_output_file_prefix.log"
"$ctrl_program" 41

# Perf analyze {{{
if [ "$perf" != 0 ]; then
  printf "Processing performace trace ...\n" 1>&2
  "$perf_program" script | "$flamegraph_stack_collapse_program" > "$output_folder/$stat_output_file_prefix.perf-folded"
  "$flamegraph_analyze_program" "$output_folder/$stat_output_file_prefix.perf-folded" > "$output_folder/$stat_output_file_prefix.svg"
  printf "SVG path: %s\n" "$output_folder/$stat_output_file_prefix.svg"
  printf "Performace analyze done\n" 1>&2
fi
# }}}

# }}}

# General termination {{{
# printf "Umounting\n" 1>&2
printf "Done\n" 2>&1
# sudo umount /dev/pmem0
# "$ctrl_program" 0
# }}}
