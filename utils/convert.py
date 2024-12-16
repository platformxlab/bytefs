import os, sys, getopt, re, json, itertools, ast, math, subprocess, copy
import numpy as np
# import operator as op

# # supported operators
# operators = {
#     ast.Add : op.add, 
#     ast.Sub : op.sub, 
#     ast.Mult: op.mul,
#     ast.Div : op.truediv, 
#     ast.Pow : op.pow,
#     ast.USub: op.neg
# }

# def eval_(node):
#     if isinstance(node, ast.Num): # <number>
#         return node.n
#     elif isinstance(node, ast.BinOp): # <left> <operator> <right>
#         if node.left == None or node.right == None:
#           return None
#         return operators[type(node.op)](eval_(node.left), eval_(node.right))
#     elif isinstance(node, ast.UnaryOp): # <operator> <operand> e.g., -1
#         return operators[type(node.op)](eval_(node.operand))
#     else:
#         print(type(node))
#         raise TypeError(node)

# def eval_expr(expr):
#     return eval_(ast.parse(expr, mode='eval').body)

### path
script_program = os.path.abspath(__file__)
script_name = os.path.basename(script_program)
script_dir = os.path.dirname(script_program)
utils_dir = script_dir

### script setting
output_description_filename = "description.settings"
output_folder_name = f"remote_output"

### input parsing
def usage():
  print(f"Usage: {script_name}:")
  print(f"Run simulation")
  print(f"  -h             print help, this message")
  print(f"  -v             verbose")
  print(f"  -t             remove all invalid output files")
  print(f"  -f             output description folder, should contain description file description.settings")
  print(f"Return values")
  print(f"  0              script terminates correctly")
  print(f"  1              invalid options")

output_folder_tag = ""
output_folder_tag_pattern = "^[0-9a-zA-Z\-]*$"
output_description_folder = None
verbose = False
try:
  opts, args = getopt.getopt(sys.argv[1:],
      "hvt:f:",
      ["help", "verbose", "output_folder_tag=", "output_description_folder="])
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
  elif arg in ("-f", "--output_description_folder"):
    output_description_folder = opt_arg
  else:
    print(f"Unhandled option <{arg}>")
    sys.exit(1)

### input validation
assert output_description_folder is not None, f"Output description folder not specified"

if len(output_folder_tag) != 0:
  output_folder_name = f"{output_folder_name}-{output_folder_tag}"
output_folder_dir = os.path.abspath(os.path.join(utils_dir, output_folder_name))
output_file = os.path.abspath(os.path.join(output_folder_dir, "stats.json"))

output_description_folder = os.path.abspath(os.path.join(utils_dir, output_description_folder))
output_description_file = os.path.abspath(os.path.join(output_description_folder, output_description_filename))

print(f"Output folder tag:  {output_folder_tag if len(output_folder_tag) else '<no tag>'}")
print(f"Output folder:      {output_folder_dir}")
print(f"Output file:        {output_file}")
print(f"Description folder: {output_description_folder}")
print(f"Description file:   {output_description_file}")

assert os.path.isfile(output_file), f"Output file <{output_file}> not found"
assert os.path.isfile(output_description_file), f"Output description file <{output_description_file}> not found"



### generic settings
generic_setting_start = {}
generic_setting_start = copy.copy(locals())
# @note: variables that overrides pervious local variables will not be included in the generic setting variables 
# generic settings BEGIN
output_file_name = "output"
post_run_action = ""
statistics_spacing = 15
dimension_setting_separator = "!"
# generic settings END
generic_settings = [k for k in locals().keys() if k not in generic_setting_start.keys()]

# dimension definition
all_dims = {
    "filesystem"  : {"ext4", "f2fs", "nova", "pmfs", "bytefs", "bytefs_cow"},
    "workload"    : {"create", "delete", "mkdir", "rmdir", "stat", "varmail", "fileserver", "webproxy", "webserver", "oltp"},
    "measurement" : {},
}

assert all([len(a.intersection(b)) == 0 for a, b in itertools.combinations(all_dims.values(), 2)]), "Dim value repeated"

all_dims_reverse = dict()
for key, vals in all_dims.items():
  for val in vals:
    all_dims_reverse[val] = key



### parse description file
print("### Output description parse START")
dim_with_selection_list = []
selection_translation_dict_list = []
dim_with_selection = dict()
selection_translation_dict = dict()
parse_state = ""

def msg_with_line_no(line_no: int, to_print: str):
  return f"{to_print} (Line {line_no})"

def add_description_section():
  ret_str = ""
  global dim_with_selection_list, dim_with_selection
  global selection_translation_dict_list, selection_translation_dict
  if len(dim_with_selection) != 0:
    dim_with_selection_list.append(copy.deepcopy(dim_with_selection))
    selection_translation_dict_list.append(selection_translation_dict)
    ret_str = f"Info: New description section recorded"
  else:
    ret_str = f"Warn: Empty description section"
  dim_with_selection = dict()
  return ret_str

all_settings = ["generic_settings", "dimension_settings", "plot_settings"]
with open(output_description_file, "r") as f:
  for line_no, line in enumerate(f):
    line_no += 1
    line = line.strip()
    if len(line) == 0:
      ### line format: empty
      continue
    if any([line.lstrip().startswith(comment_str) for comment_str in ["#", "//"]]):
      ### line format: comment
      continue
    if any([line in all_settings]):
      ### line format: parse state change
      new_state = line
      if new_state != parse_state:
        # state change specific
        if parse_state == "dimension_settings":
          ret_str = add_description_section()
          print(f"""  {msg_with_line_no(line_no, ret_str)}""")
        # state change generic
        parse_state = new_state
        print(f""" {msg_with_line_no(line_no, f"Info: Parse state change to {parse_state}")}""")
      else:
        print(f""" {msg_with_line_no(line_no, f"Warn: Parse state already in {parse_state}")}""")
      continue
    if parse_state == "generic_settings":
      ### parse state: settings
      if ":" in line:
        components = line.split(":")
        assert len(components) == 2, f"""Invalid generic setting <{line}>, no ":" present"""
        setting = components[0].strip()
        value = components[1].strip()
        assert setting in generic_settings, f"Setting <{setting}> not found in list {generic_settings}"
        print(f"  Change setting {setting}: {value}")
        exec(f"""{setting} = "{value}" """)
    elif parse_state == "dimension_settings":
      if line in ["!", "EOS", dimension_setting_separator]:
        ret_str = add_description_section()
        print(f"""  {msg_with_line_no(line_no, ret_str)}""")
        continue
      if ":" in line:
        components = line.split(":")
        assert len(components) == 2, f"""Invalid output description <{line}>, only less than or equal to one ":" is accepted"""
        dim = components[0].strip()
        assert dim in all_dims.keys(), f"Dimension <{dim}> does not exist"
        dim_selections_list = [selection.strip() for selection in components[1].split(",")]
        for dim_idx, dim_selection in enumerate(dim_selections_list):
          if "|" in dim_selection:
            assert dim_selection.count("|") <= 1, f"Invalid selection format <{dim_selection}>, more than one | detected"
            selection_formula, translated_name = [name.strip() for name in dim_selection.split("|")]
            selection_translation_dict[f"{len(selection_translation_dict_list)}|{dim}|{selection_formula}"] = translated_name
            dim_selections_list[dim_idx] = selection_formula
      else:
        dim = line.strip()
        assert dim in all_dims.keys(), f"Dimension <{dim}> does not exist"
        dim_selections_list = all_dims[dim]
      dim_selections_list = [(
          [node.id for node in ast.walk(ast.parse(dim_selection)) if isinstance(node, ast.Name)],
          dim_selection
        ) for dim_selection in dim_selections_list]
      dim_with_selection[dim] = dim_selections_list
    elif parse_state == "plot_settings":
      pass
    else:
      if parse_state in all_settings:
        raise NotImplementedError(f"Parse state <{parse_state}> logic not implemented")
      print(f"""  {msg_with_line_no(line_no, f"Line #{line_no} Err: Invalid parse state <{parse_state}>, ignoring section")}""")
ret_str = add_description_section()
print(f"""  {msg_with_line_no(line_no, ret_str)}""")
print("### Output description parse END")



### parse dataset
print("### Dataset parse BEGIN")
dataset = None
with open(output_file, "r") as f:
  dataset = json.load(f)

def find_dims(dataset: dict):
  if type(dataset) is not dict or len(dataset) == 0:
    return [set()]
  current_dim = set(dataset.keys())
  children_dims = None
  for val in dataset.values():
    current_children_dims = find_dims(val)
    if children_dims is None:
      children_dims = current_children_dims
    else:
      if len(children_dims) < len(current_children_dims):
        children_dims, current_children_dims = current_children_dims, children_dims
      for children_dim_idx in range(len(current_children_dims)):
        children_dims[children_dim_idx] = children_dims[children_dim_idx].union(current_children_dims[children_dim_idx])
  if len(children_dims) == 1 and len(children_dims[0]) == 0:
    return [current_dim]
  return [current_dim, *children_dims]

datafile_dims = find_dims(dataset)
suggested_dims = [next(iter(datafile_dim)) for datafile_dim in datafile_dims]
suggested_dims = [
    all_dims_reverse[suggested_dim] if suggested_dim in all_dims_reverse else None for suggested_dim in suggested_dims
]

print("Datafile description:")
for key, value in zip(suggested_dims, datafile_dims):
  print(f"""  {key if key is not None else "<implicit dimension>":>20s}: {sorted(list(value))}""")

assert all([
    all([
        feature in all_dims_reverse and all_dims_reverse[feature] == suggested_dims[dim_idx] for feature in datafile_dims[dim_idx]
    ]) if suggested_dims[dim_idx] != None else True for dim_idx in range(len(datafile_dims))
]), f"Datafile contain something not in all_dims, check data file"
num_implicit_dims = sum([suggested_dim == None for suggested_dim in suggested_dims])
assert num_implicit_dims, f"Number of implicit dimension is <{num_implicit_dims}>, should not be greater than 1"
symmetric_diff = set(all_dims.keys()).symmetric_difference(set([
    suggested_dim for suggested_dim in suggested_dims if suggested_dim is not None
]))
assert len(symmetric_diff) <= 1, "Invalid implicit dimension"
suggested_dims = [
    suggested_dim if suggested_dim is not None else symmetric_diff.pop() for suggested_dim in suggested_dims
]
assert len(set(suggested_dims)) == len(suggested_dims), "Dimension overlap, check data file"
print("### Dataset parse END")



### generate output data file
assert len(dim_with_selection_list) > 0, f"No section exist in description"
total_dim_num_list = [len(dim_with_selection) for dim_with_selection in dim_with_selection_list]
assert all([total_dim_num == total_dim_num_list[0] for total_dim_num in total_dim_num_list]), f"Description contains sections of unequal dimension: {total_dim_num_list}"
total_dim_num = total_dim_num_list[0]
if len(dim_with_selection) > 3:
  print(f"Unsupported dimension <{len(dim_with_selection)}>")

print("Translation dict:")
for group_idx, selection_translation_dict in enumerate(selection_translation_dict_list):
  translation_dict_to_print = {k : v for k, v in selection_translation_dict.items() if k.startswith(f"{group_idx}|")}
  print(f"""  {f"Group {group_idx}":>20s}: {translation_dict_to_print}""")
# assert all([selection_translation_dict.values() == selection_translation_dict_list[0].values() for selection_translation_dict in selection_translation_dict_list])

print("Parsed description:")
for group_idx, dim_with_selection in enumerate(dim_with_selection_list):
  print(f" Group {group_idx}")
  for key, value in dim_with_selection.items():
    print(f"""  {key:>20s}: {[formula_tuple[1] for formula_tuple in sorted(list(value))]}""")

unified_dim_names, unified_dim_translations_list = ["" for _ in range(total_dim_num)], [[] for _ in range(total_dim_num)]
for group_idx, (dim_with_selection, selection_translation_dict) in enumerate(zip(dim_with_selection_list, selection_translation_dict_list)):
  dim_names, dim_selections_list, dim_translations_list = [], [], []
  for dim_idx in range(total_dim_num):
    dim_name, dim_selections = list(dim_with_selection.items())[dim_idx]
    dim_translations = [
        dim_selection if f"{group_idx}|{dim_name}|{dim_selection}" not in selection_translation_dict else 
        selection_translation_dict[f"{group_idx}|{dim_name}|{dim_selection}"] 
        for dim_selection in [formula_tuple[1] for formula_tuple in dim_selections]
    ]
    if len(unified_dim_names[dim_idx]) == 0:
      unified_dim_names[dim_idx] = dim_name
    else:
      assert unified_dim_names[dim_idx] == dim_name, f"Dim name list mismatch in group {group_idx}"
    for dim_selection, dim_translation in zip(dim_selections, dim_translations):
      if dim_translation not in unified_dim_translations_list[dim_idx]:
        unified_dim_translations_list[dim_idx].append(dim_translation)

print(" Unified dim:")
print(f"""  {"Dim names":>20s}: {unified_dim_names}""")
print(f"""  {"Dim translations":>20s}: {unified_dim_translations_list}""")



### output generation
print("### Output generation BEGIN")
output_str = ""
if total_dim_num == 1:
  raise NotImplementedError()
elif total_dim_num == 2:
  raise NotImplementedError()
elif total_dim_num == 3:
  print(" Generating data matrix")
  output_data_matrix = np.zeros([
    len(unified_dim_translations_list[0]), len(unified_dim_translations_list[1]), len(unified_dim_translations_list[2])
  ])
  for group_idx, dim_with_selection in enumerate(dim_with_selection_list):
    for dim0_selection_idx, dim0_selection in enumerate(dim_with_selection[unified_dim_names[0]]):
      for dim1_selection_idx, dim1_selection in enumerate(dim_with_selection[unified_dim_names[1]]):
        for dim2_selection_idx, dim2_selection in enumerate(dim_with_selection[unified_dim_names[2]]):
          dim_selections = [dim0_selection, dim1_selection, dim2_selection]
          dim_idxs = [suggested_dims.index(dim) for dim in unified_dim_names]
          dim_labels = [dim_selections[i] for i in dim_idxs]
          dim_variables_list, dim_formulas = [], []
          for dim_label in dim_labels:
            dim_variables_list.append(dim_label[0])
            dim_formulas.append(dim_label[1])
          def recursive_eval(dataset: dict, previous_dims: list, current_dim_idx: int):
            if current_dim_idx == total_dim_num:
              if previous_dims[0] in dataset and \
                 previous_dims[1] in dataset[previous_dims[0]] and \
                 previous_dims[2] in dataset[previous_dims[0]][previous_dims[1]]:
                return dataset[previous_dims[0]][previous_dims[1]][previous_dims[2]]
              return math.nan
            dim_variables = dim_variables_list[current_dim_idx]
            dim_formula = dim_formulas[current_dim_idx]
            dim_variable_vals = [
              recursive_eval(dataset, [*previous_dims, dim_variable], current_dim_idx + 1)
              for dim_variable in dim_variables
            ]
            for dim_variable, dim_variable_val in zip(dim_variables, dim_variable_vals):
              dim_formula = dim_formula.replace(
                  dim_variable, "math.nan" if math.isnan(float(dim_variable_val)) else str(dim_variable_val)
              )
            return eval(dim_formula)
          translation_keys = [
              f"{group_idx}|{unified_dim_names[dim_idx]}|{selection_formula}" \
              for dim_idx, (_, selection_formula) in enumerate(dim_selections)
          ]
          translations = [
              selection_translation_dict[translation_key] if translation_key in selection_translation_dict else \
              selection_formula for translation_key, (_, selection_formula) in zip(translation_keys, dim_selections)
          ]
          output_data_matrix[
              tuple([unified_dim_translations_list[i].index(translations[i]) for i in range(len(unified_dim_names))])
          ] = recursive_eval(dataset, [], 0)
  dim2_fieldlen = max([len(dim_translation) for dim_translation in unified_dim_translations_list[-1]]) + 2
    
  print(" Generating output file")
  output_str += "{0:{1}s}".format("", dim2_fieldlen)
  output_str += "".join(["{0:>{1}s} ".format(l, statistics_spacing) for l in unified_dim_translations_list[0]]) + "\n\n"
  for dim1_selection_idx, dim1_selection in enumerate(unified_dim_translations_list[1]):
    output_str += "{0}\n".format(unified_dim_translations_list[1][dim1_selection_idx])
    for dim2_selection_idx, dim2_selection in enumerate(unified_dim_translations_list[2]):
      output_str += "{0:{1}s}".format(unified_dim_translations_list[2][dim2_selection_idx], dim2_fieldlen)
      for dim0_selection_idx, dim0_selection in enumerate(unified_dim_translations_list[0]):
        result = recursive_eval(dataset, [], 0)
        output_str += "{0:{1}.2f} ".format(output_data_matrix[dim0_selection_idx, dim1_selection_idx, dim2_selection_idx], statistics_spacing)
      output_str += "\n"
    output_str += "\n"

with open(os.path.join(output_description_folder, output_file_name), "w") as f:
  f.write(output_str)
print("### Output generation END")



### post run action
if post_run_action != "":
  print("### Post-run action BEGIN")
  print(f"Executing <{post_run_action}> @ <{output_description_folder}>")
  s = subprocess.Popen(post_run_action, cwd=output_description_folder, shell=True)
  s.wait()
  print("### Post-run action END")

