in_file_name="logs/perl/context/trace.out"
# out_file="m5out4/trace_pc.out"
# out_file2="m5out/trace_seq.out"
golden_file="logs/perl/no_lvp/trace.out"
# golden_out = "m5ou2/trace_golden.out"

def read_file(file_name):
    lines = open(file_name).readlines()

    pc_lines = []
    seq_nums = []
    cycles = []
    mispredict_nums = set()
    rob_entries = []

    for i, line in enumerate(lines):
        if "Committing instruction with PC" in line:
            pc = line[line.index("(")+1:line.index("=")]
            seq_num = line.split(" ")[3].strip()[4:-1]
            pc_lines.append(pc)
            seq_nums.append(seq_num)
            cycles.append(int(line.split(":")[0].strip()) // 500)
        if "Mispredicted" in line:
            mispredict_nums.add(line[line.index("[")+1:line.index("]")])
            rob_entries.append(int(line.split(" ")[-1].strip()))
            
    return pc_lines, seq_nums, cycles, mispredict_nums, rob_entries

pc_lines, seq_nums, cycles, mispredict_nums, rob_entries = read_file(in_file_name)

print(f"Number of mispredicts: {len(mispredict_nums)}")
print(f"Number of rob entries: {len(rob_entries)}")

golden_pc_lines, golden_seq_nums, golden_cycles, _, _ = read_file(golden_file)

# # write pc_lines to new file
# with open(out_file, 'w') as f:
#     for i, item in enumerate(pc_lines):
#         f.write(f"{item} {cycles[i]}")
#         if seq_nums[i] in mispredict_nums:
#             f.write(" val_mispredict\n")
#         else:
#             f.write("\n")

# # write golden to file
# with open(golden_out, 'w') as f:
#     for i, item in enumerate(golden_pc_lines):
#         f.write(f"{item} {golden_cycles[i]}\n")

# calc average val mispredict penalty
penalties = []
rob_counter = 0
for i, item in enumerate(pc_lines):
    if seq_nums[i] in mispredict_nums:
        # we check the last rob entry since that guy was the last one squashed
        
        penalties.append((cycles[i+rob_entries[rob_counter]] - cycles[i]) - (golden_cycles[i+rob_entries[rob_counter]] - golden_cycles[i]))
        rob_counter += 1

print(f"Average mispredict penalty: {sum(penalties) / len(penalties)}")
