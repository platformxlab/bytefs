import os, sys, getopt, re, copy, json, dictdiffer

script_program = os.path.abspath(__file__)
script_name = os.path.basename(script_program)
script_dir = os.path.dirname(script_program)
utils_dir = script_dir

def usage():
  print(f"Usage: {script_name}:")
  print(f"Run simulation")
  print(f"  -h             print help, this message")
  print(f"  -v             verbose")
  print(f"  -t             remove all invalid output files")
  print(f"Return values")
  print(f"  0              script terminates correctly")
  print(f"  1              invalid options")

output_folder_tag = ""
output_folder_tag_pattern = "^[0-9a-zA-Z\-]*$"
verbose = False
try:
  opts, args = getopt.getopt(sys.argv[1:], "hvt:", ["help", "verbose", "output_folder_tag="])
except getopt.GetoptError as err:
  # print help information and exit:
  print(err)  # will print something like "option -a not recognized"
  usage()
  sys.exit(1)
for arg, opt_arg in opts:
  if arg in ("-h", "--help"):
    usage()
    sys.exit()
  elif arg in ("-v", "--verbose"):
    verbose = True
  elif arg in ("-t", "--output_folder_tag"):
    output_folder_tag = opt_arg
    if re.match(output_folder_tag_pattern, output_folder_tag) is None:
      print(f"{output_folder_tag_pattern} does not match regex {output_folder_tag_pattern}")
  else:
    print(f"Unhandled option <{arg}>")
    sys.exit(1)

output_folder_name = f"remote_output"
if len(output_folder_tag) != 0:
  output_folder_name = f"{output_folder_name}-{output_folder_tag}"
output_folder_dir = os.path.join(utils_dir, output_folder_name)

print(f"Output folder tag: {output_folder_tag if len(output_folder_tag) else '<no tag>'}")
print(f"Output folder:     {output_folder_dir}")

assert os.path.isdir(output_folder_dir), f"Output folder <{output_folder_dir}> not found"

filter_measurement_set = {"logfile_populated", "datafiles_populated", "svg_path"}
filesystems = set()
workloads = set()
measurements = set()
dataset = dict()
for filename in os.listdir(output_folder_dir):
  # remove generated file from gathering
  if filename == "stats.json":
    continue
  filename_split = filename.split(".")
  filesystem = filename_split[0].strip()
  workload = filename_split[1].strip()
  filesystems.add(filesystem)
  workloads.add(workload)
  if filesystem not in dataset:
    dataset[filesystem] = dict()
  if workload not in dataset[filesystem]:
    dataset[filesystem][workload] = dict()
  file_abs_path = os.path.join(output_folder_dir, filename)
  with open(file_abs_path, "r") as f:
    stat_start = False
    for line in f:
      if "Running..." in line:
        stat_start = True
      if stat_start:
        result = re.match("(?:.*:)?\s*([a-zA-Z0-9_ ]*):\s*(.*)\s*", line)
        measurement_name = ""
        if result is not None and len(result.groups()) >= 2:
          measurement_name = " ".join(result.group(1).strip().split()).replace(" ", "_").lower()
          measurement_val = re.findall(r'\d+\.?\d*', result.group(2))
        elif "Run took" in line:
          result = re.match("(?:[\d\.]+:)?\s*(.*)", line)
          if result is not None:
            line = result.group(1)
          run_time = re.findall(r'\d+\.?\d*', line)
          if len(run_time) == 1:
            run_time = float(run_time[0])
          measurement_name = "ops"
          measurement_val = 100
          measurement_name = "ops_throughput"
          measurement_val = str(measurement_val / run_time)
        if len(measurement_name) != 0: 
          dataset_begin = copy.deepcopy(dataset)
          # special parsing BEGIN
          if measurement_name in filter_measurement_set:
            pass
          elif measurement_name == "io_summary":
            dataset[filesystem][workload]["ops"] = measurement_val[0]
            dataset[filesystem][workload]["ops_throughput"] = measurement_val[1]
          # special parsing END
          # generic parsing BEGIN
          else:
            if len(measurement_val) == 1:
              dataset[filesystem][workload][measurement_name] = measurement_val[0]
            elif len(measurement_val) == 3 and all([pattern in line for pattern in ["R:", "W:"]]):
              dataset[filesystem][workload][f"{measurement_name}_total"] = measurement_val[0]
              dataset[filesystem][workload][f"{measurement_name}_read"]  = measurement_val[1]
              dataset[filesystem][workload][f"{measurement_name}_write"] = measurement_val[2]
            elif len(measurement_val) == 3 and all([pattern in line for pattern in ["Hit:", "Miss:"]]):
              dataset[filesystem][workload][f"{measurement_name}_total"] = measurement_val[0]
              dataset[filesystem][workload][f"{measurement_name}_hit"]   = measurement_val[1]
              dataset[filesystem][workload][f"{measurement_name}_miss"]  = measurement_val[2]
          # generic parsing END

          # Debug diff
          # print("======")
          # print(line.strip())
          # for diff in list(dictdiffer.diff(dataset_begin, dataset)):         
          #   print(diff)
with open(os.path.join(output_folder_name, "stats.json"), "w") as f:
  json.dump(dataset, f, indent=2)

