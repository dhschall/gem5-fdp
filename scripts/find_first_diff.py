def parse_blocks(filename):
    """Reads a file and extracts blocks of 16 lines, ignoring the first number."""
    blocks = []
    true_blocks = []
    with open(filename, 'r') as file:
        lines = file.readlines()
        for i in range(0, len(lines), 16):
            if i + 16 <= len(lines):
                # Remove the first number from each line in the block
                block = [line[line.index(":")+1:].strip() for line in lines[i:i+16]]
                blocks.append(block)
                true_blocks.append(line.strip() for line in lines[i:i+16])
    return blocks, true_blocks

def find_unique_block(blocks1, blocks2):
    """Finds the first block in file1 that does not appear in file2."""

    # Check for the first block in file1 that is not in file2
    i = 0
    for block in blocks1.__reversed__():
        i += 1
        if (i % 10000) == 0:
            print("Checking block", i)
        if block in blocks2:
            return block
    return None

# Files to compare
file1 = "m5out1/trace.out"
file2 = "m5out/trace.out"

# Parse the blocks
blocks1, true_blocks1 = parse_blocks(file1)
blocks2, true_blocks2 = parse_blocks(file2)

print("blocks parsed")

# Find the unique block
unique_block = find_unique_block(blocks1, blocks2)

if unique_block:
    true_block_ind = blocks1.index(unique_block)
    true_block = true_blocks1[true_block_ind]
    print("First unique block found in file1:")
    for line in true_block:
        print(line)
else:
    print("No unique block found in file1.")
