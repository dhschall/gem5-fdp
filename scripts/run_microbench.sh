#!/bin/bash

# Check if a benchmark name is provided as an argument
if [ -z "$1" ]; then
    echo "Please specify the name of the microbenchmark to run (e.g., bench)."
    exit 1
fi

if [-z "$2" ]; then
    echo "Please specify the output directory."
    exit 1
fi

BENCHMARK_NAME=$1
OUTPUT_DIR=$2

# Assemble the microbenchmark
nasm -f elf32 -o microbenchmarks/object_files/${BENCHMARK_NAME}.o microbenchmarks/src/${BENCHMARK_NAME}.asm
ld -m elf_i386 -o microbenchmarks/bin/${BENCHMARK_NAME} microbenchmarks/object_files/${BENCHMARK_NAME}.o

# Run the microbenchmark in gem5
build/X86/gem5.opt --debug-flags=O3PipeView,O3CPUAll,Cache,LVPAll,DumpRegs --debug-file=trace.out --outdir=${OUTPUT_DIR} configs/deprecated/example/new_se.py --cpu-type=X86O3CPU --lvp=True --scalar=True --cmd=/home/rapiduser/gem5VP/microbenchmarks/bin/${BENCHMARK_NAME} --caches --l2cache --l1d_size=64 --l1d_assoc=1 --l2_size=1MB --l2_assoc=1

# Generate the pipeline view
./util/o3-pipeview.py -c 500 -o pipeview.out --color ${OUTPUT_DIR}/trace.out

# Display the filtered output
cat pipeview.out
