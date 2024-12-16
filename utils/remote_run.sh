#!/bin/bash

# Script settings {{{
trap "echo; exit" INT
set -o pipefail
# set -x
# }}}

# Color strings {{{
RED='\033[0;31m'
GREEN='\033[0;32m'
ORANGE='\033[1;33m'
CYAN='\033[0;36m'
ENDC='\033[0m'
# }}}

# Folder absolute paths {{{
utils_dir="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
base_dir="$( dirname -- "$utils_dir" )"
figure_drawing_dir="$base_dir/figure_drawing"
source "$utils_dir/bash_utils.sh"
# }}}

# Script configs {{{
wait_time_before_start=5
# }}}

# Setting variables {{{
output_folder_tag=""
remote_target=""
vm_target=""
perf=0
perf_only=0
dry_run=0
# }}}

# Default settings {{{
declare -A workloads_sets
workloads_sets=(
  ["filebench"]="create delete mkdir rmdir stat varmail fileserver webserver webproxy oltp"
  ["ycsb"]="workloada workloadb workloadc workloadd workloade workloadf"
)
workloads=()
for workloads_set in "${!workloads_sets[@]}"; do
  while IFS=' ' read -ra workloads_per_set; do
    for workload in "${workloads_per_set[@]}"; do
      workloads+=( "$workload" )
    done
  done <<< "${workloads_sets[$workloads_set]}"
done
filesystems=( nova pmfs f2fs ext4 bytefs )

declare -A workloads_set
for workload in "${workloads[@]}"; do
  workloads_set["$workload"]=""
done
declare -A filesystems_set
for filesystem in "${filesystems[@]}"; do
  filesystems_set["$filesystem"]=""
done
# }}}

# Parse command line args {{{
while getopts "hw:s:f:t:r:v:pod" arg; do
  case $arg in
    f)
      filesystems=()
      while IFS=',' read -ra selected_filesystems; do
        for filesystem in "${selected_filesystems[@]}"; do
          ! [[ -v "filesystems_set[$filesystem]" ]]
          assert_exit $? "<%s> is not a candidate filesystem (%s)\n" "$filesystem" "${!filesystems_set[*]}"
          filesystems+=( "$filesystem" )
        done
      done <<< "${OPTARG}"
      ;;
    w)
      workloads=()
      while IFS=',' read -ra selected_workloads; do
        for workload in "${selected_workloads[@]}"; do
          ! [[ -v "workloads_set[$workload]" ]]
          assert_exit $? "<%s> is not a candidate workload (%s)\n" "$workload" "${!workloads_set[*]}"
          workloads+=( "$workload" )
        done
      done <<< "${OPTARG}"
      ;;
    s)
      workloads=()
      workload_set_name="${OPTARG}"
      ! [[ -v "workloads_sets[$workload_set_name]" ]]
      assert_exit $? "<%s> is not a candidate workload set (%s)\n" "$workload_set_name" "${!workloads_sets[*]}"
      workloads_per_set=()
      while IFS=' ' read -ra workloads_per_set; do
        for workload in "${workloads_per_set[@]}"; do
          workloads+=( "$workload" )
        done
      done <<< "${workloads_sets[$workload_set_name]}"
      ;;
    t)
      output_folder_tag=${OPTARG}
      if ! [[ "$output_folder_tag" =~ ^[0-9a-zA-Z\-]*$ ]]; then
        printf "Tag must match regex ^[0-9a-zA-Z\-]*$, which <%s> does not\n" "$output_folder_tag"
        exit 1
      fi
      ;;
    r)
      remote_target=${OPTARG}
      ;;
    v)
      vm_target=${OPTARG}
      ;;
    p)
      perf=1
      ;;
    o)
      perf_only=1
      ;;
    d)
      dry_run=1
      ;;
    h)
      printf "Useage: %s:\n" "$0" >&2
      printf "  -f FS1[,FS2,...]  target filesystem(s)\n" >&2
      printf "  -f FS1[,FS2,...]  target filesystem(s)\n" >&2
      printf "  -w WL1[,WL2,...]  target workload(s)\n" >&2
      printf "\n" >&2
      printf "  -t OUTPUT_FOLDER  output folder postfix\n" >&2
      printf "\n" >&2
      printf "  -r REMOTE_TARGET  target remote name\n" >&2
      printf "  -v VM_TARGET      target vm name\n" >&2
      printf "\n" >&2
      printf "  -p                also do a perf run in addition to normal run\n" >&2
      printf "  -po               only do the perf run, no normal run\n" >&2
      printf "  -d                dry run, output what the script is going to do\n" >&2
      printf "                    without actual action\n" >&2
      exit 0
      ;;
    *)
      exit 1
      ;;
  esac
done
printf "Filesystem(s):     %s\n" "${filesystems[*]}"
printf "Workload(s):       %s\n" "${workloads[*]}"
printf "Perf:              %s\n" "$perf"
printf "Perf Only:         %s\n" "$perf_only"
printf "Dry Run:           %s\n" "$dry_run"
printf "Remote Target:     %s\n" "$remote_target"
printf "VM Target:         %s\n" "$vm_target"
if [[ $perf_only -ne 0 ]] && [[ $perf -eq 0 ]]; then
  printf "${RED}Invalid configuration, perf_only is asserted while perf is not${ENDC}\n" >&2
  exit 1
fi
# }}}

# Dir configuration {{{
# TODO: change this hard-coded "$remote_base_dir" with find
remote_base_dir=/home/csl/Byte_fs
remote_utils_dir="$remote_base_dir/utils"
output_dir="$figure_drawing_dir/remote_output"
if [ -n "$output_folder_tag" ]; then
  output_dir="$output_dir/$output_folder_tag"
else
  output_dir="$output_dir/default"
fi

printf "Output Folder Tag: %s\n" "$output_folder_tag"
printf "Local Out Folder:  %s\n" "$(basename -- $output_dir)"
printf "  [Full Path]:     %s\n" "$output_dir"
if [ -d "$output_dir" ] && [ -n "$(ls -A "$output_dir")" ]; then
  printf "${ORANGE}Non-empty output folder already exists, silent overwrite might occur${ENDC}\n" >&2
fi
# }}}

# Remote configuration {{{
vm_name="ubuntu18.04"
remote_run_program="$remote_utils_dir/filebench_run.sh"
# }}}

# Local variable initialization {{{
all_runs=$(( "${#workloads[@]}" * "${#filesystems[@]}" ))
if [[ "$perf" -ne 0 ]] && [[ "$perf_only" -eq 0 ]]; then
  all_runs=$(( "$all_runs" * 2 ))
fi
printf "Total runs: %d\n" "$all_runs"

current_runs=0
retval=0
# }}}

# Start status verify {{{
# check output log status
for workload in "${workloads[@]}"; do
  for filesystem in "${filesystems[@]}"; do
    if [[ -f "$output_dir/$filesystem.$workload" ]]; then
      current_runs=$(( "$current_runs" + 1 ))
    fi
    if [ "$perf" -ne 0 ] && [[ -f "$output_dir/$filesystem.$workload.perf" ]]; then
      current_runs=$(( "$current_runs" + 1 ))
    fi
  done
done
printf "Finished runs: %d\n" "$current_runs" >&2
if [ "$current_runs" -ge "$all_runs" ]; then
  # done, exit
  printf "${GREEN}All workloads profiled${ENDC}\n" >&2
  exit 0
else
  # remote status examine
  ssh "$remote_target" -q -o ConnectTimeOut=5 -t "echo" > /dev/null 2>&1
  assert_zero_exit "$?" "Remote target <$remote_target> does not exist or is not responding\n"
  # ssh "$vm_target" -q -o ConnectTimeOut=5 -t "echo" > /dev/null 2>&1
  # assert_zero_exit "$?" "VM target <$vm_target> does not exist or is not responding\n"
  remote_vm_list=$(ssh "$remote_target" -q -o ConnectTimeOut=5 -t "sudo virsh list --all")
  assert_zero_exit "$?" "Remote target <$remote_target> does not exist or is not responding\n"
  vm_status="$(grep "$vm_name" <<< "$remote_vm_list")"
  assert_zero_exit "$?" "VM <$vm_name> does not exist in the list:\n$(printf "%b" "$remote_vm_list" | sed -e "s/^/  /")\n"
  printf "Connection check finished\n" >&2
  display_time
  pretty_countdown "${GREEN}Run will start in %d sec...${ENDC}" "$wait_time_before_start" >&2
  if ! grep "running" <<< "$vm_status" > /dev/null 2>&1; then
    printf "VM target not in running state, starting ...\n"
    ssh "$remote_target" -q -t "sudo virsh destroy $vm_name > /dev/null 2>&1; sudo virsh start $vm_name" 2>/dev/null | sed -e "s/^/  /"
    printf "Boot command sent\n"
    sleep 20
  elif [ "$dry_run" -eq 0 ]; then
    printf "VM target running, rebooting ...\n"
    ssh "$remote_target" -q -t "sudo virsh destroy $vm_name; sudo virsh start $vm_name" 2>/dev/null | sed -e "s/^/  /"
    printf "Boot command sent\n"
    sleep 20
  fi
  retval=1
  retry_num=1
  while [ "$retval" -ne 0 ]; do
    ssh "$vm_target" -q -o ConnectTimeOut=5 -t "echo" > /dev/null 2>&1
    retval=$?
    if [ "$retval" -ne 0 ]; then printf "${RED}Retrying [%d] ...${ENDC}\n" "$retry_num"; fi
    retry_num="$(( "$retry_num" + 1 ))"
    sleep 5
  done
  printf "Target started\n"
  ssh "$vm_target" -t "make -C $remote_utils_dir clean && make -C $remote_utils_dir"
  printf "Target initialized\n"
  mkdir -p "$output_dir"
fi
# }}}

# Run benchmark helper function {{{
run_benchmark() {
  local filesystem=$1
  local workload="$2"
  local perf=$3
  # printf "filesystem $filesystem workload $workload perf $perf\n"
  printf "${GREEN}Starting run on filesystem <%s> workload <%s>" "$filesystem" "$workload" >&2
  if [ "$perf" -ne 0 ]; then printf " [Perf]"; fi
  printf "${ENDC}\n"
  # appending auxiliary arguments
  aux_args=""
  if [ "$perf" -ne 0 ];           then aux_args="$aux_args -p"; fi
  if [ "$dry_run" -ne 0 ];        then aux_args="$aux_args -d"; fi
  if [ -n "$output_folder_tag" ]; then aux_args="$aux_args -t $output_folder_tag"; fi
  printf "Auxiliary args: <%s>\n" "$(echo $aux_args | xargs)" >&2
  # assign target name
  target_file_prefix="$output_dir/$filesystem.$workload"
  if [ "$perf" -ne 0 ]; then
    target_file_prefix="$target_file_prefix.perf"
  fi
  # run benchmark
  retval=1
  while [ "$retval" -ne 0 ]; do
    ssh "$vm_target" -o ConnectTimeOut=30 -q -t \
        "$remote_run_program" -w "$workload" -f "$filesystem" $aux_args | \
        tee "$target_file_prefix" | sed -e "s/^/  /"
    retval=$?
    if [ "$retval" -ne 0 ]; then printf "${RED}Retrying ...${ENDC}\n"; fi
    if [ "$retval" -ne 0 ] || [ "$dry_run" -ne 0 ]; then rm "$target_file_prefix"; fi
    sleep 5
  done
  # statistics
  current_runs=$(( "$current_runs" + 1 ))
  printf "${GREEN}Current/Total Runs [%3d / %3d = %5.2f%%]${ENDC}\n" "$current_runs" "$all_runs" "$(echo "scale=10; $current_runs / $all_runs * 100" | bc)" >&2
  # perf scp result
  if [ "$perf" -ne 0 ]; then
    remote_svg_path="$(grep "SVG path" < "$target_file_prefix" | sed -rn "s/^.*:\s*(.*)/\1/p" | sed -rn "s/[\s\r]*$//p")"
    printf "${CYAN}Copying remote svg at %s...${ENDC}\n" "$remote_svg_path" >&2
    scp "$vm_target:$remote_svg_path" "$target_file_prefix.svg"
  fi
  # reboot via parent node
  if [ "$dry_run" -eq 0 ]; then
    ssh "$remote_target" -t "sudo virsh destroy $vm_name > /dev/null 2>&1; sudo virsh start $vm_name > /dev/null 2>&1"
    printf "${CYAN}Waiting for remote to reboot ...${ENDC}\n" >&2
    sleep 20
  fi
}
# }}}

# Run all workloads under all filesystems {{{
for workload in "${workloads[@]}"; do
  for filesystem in "${filesystems[@]}"; do
    if [ "$perf_only" -eq 0 ]; then
      # run workload normally
      if [[ -f "$output_dir/$filesystem.$workload" ]]; then
        printf "${GREEN}Skipping %s.%s${ENDC}\n" "$filesystem" "$workload"
      else
        run_benchmark "$filesystem" "$workload" 0
      fi
    fi

    if [ "$perf" -ne 0 ]; then
      # run workload with performance analyzing on
      if [[ -f "$output_dir/$filesystem.$workload.perf" ]]; then
        printf "${GREEN}Skipping %s.%s [Perf]${ENDC}\n" "$filesystem" "$workload"
      else
        run_benchmark "$filesystem" "$workload" 1
      fi
    fi
  done
done
# }}}

# Finish {{{
display_time
# }}}

