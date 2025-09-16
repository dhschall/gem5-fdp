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

# compile the benchmark
gcc -static -o microbenchmarks/bin/${BENCHMARK_NAME} microbenchmarks/src/${BENCHMARK_NAME}.c

# Run the microbenchmark in gem5
build/X86/gem5.opt --outdir=${OUTPUT_DIR} configs/deprecated/example/new_se.py --cpu-type=X86O3CPU --lvp=True --cmd=/home/rapiduser/gem5VP/microbenchmarks/bin/${BENCHMARK_NAME} --caches --l1d_size=64 --l1d_assoc=1 --l2_size=1MB --l2_assoc=1

