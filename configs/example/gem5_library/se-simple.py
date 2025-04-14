# Copyright (c) 2024 Technical University of Munich
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

"""
Simple gem5 configuration script to run a binary in syscall emulation mode.
Usage: ./build/<ISA>/gem5.opt se-imple.py <BINARY_PATH> <ARG1> <ARG2> ...

"""

import argparse

from m5.objects import (
    LTAGE,
    L2XBar,
    TaggedPrefetcher,
)

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.caches.l1dcache import L1DCache
from gem5.components.cachehierarchies.classic.caches.l1icache import L1ICache
from gem5.components.cachehierarchies.classic.caches.mmu_cache import MMUCache
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory.dram_interfaces.ddr4 import DDR4_2400_8x8
from gem5.components.memory.memory import ChanneledMemory
from gem5.components.processors.base_cpu_processor import BaseCPUProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_core import SimpleCore
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import (
    BinaryResource,
    obtain_resource,
)
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

isa_choices = {
    "X86": ISA.X86,
    "Arm": ISA.ARM,
    "RiscV": ISA.RISCV,
}

cpu_types = {
    "atomic": CPUTypes.ATOMIC,
    "timing": CPUTypes.TIMING,
    "o3": CPUTypes.O3,
}

parser = argparse.ArgumentParser(
    description="An example configuration script to run a binary in system call emulation"
)

# The only positional argument accepted is the benchmark name in this script.


parser.add_argument(
    "--ftNum",
    type=int,
    default=1,
    help="Number of fetch targets",
)

parser.add_argument(
    "--isa",
    type=str,
    default="Arm",
    help="The ISA to simulate.",
    choices=isa_choices.keys(),
)

parser.add_argument(
    "--cpu-type",
    type=str,
    default="o3",
    help="The CPU model to use.",
    choices=cpu_types.keys(),
)

parser.add_argument(
    "--pfc",
    action="store_true",
    default=False,
    help="Enable PFC",
)

parser.add_argument("cmd", nargs=argparse.REMAINDER)

args = parser.parse_args()


# This check ensures the gem5 binary is compiled to the correct ISA target.
# If not, an exception will be thrown.
requires(isa_required=isa_choices[args.isa])


cacheline_size = 128

# We use a single channel DDR3_1600 memory system
memory = ChanneledMemory(DDR4_2400_8x8, 2, cacheline_size, size="3GiB")

# We use a PrivateL1PrivateL2CacheHierarchy with 32kB L1 caches and 256kB L2
cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32kB", l1i_size="32kB", l2_size="512kB"
)


# Create the processor with one core
processor = SimpleProcessor(
    cpu_type=cpu_types[args.cpu_type], isa=isa_choices[args.isa], num_cores=1
)

cpu = processor.cores[-1].core


from m5.objects import (  # TageSCLRef,; ITTAGE,; ITTAGE_TAGE,
    LTAGE,
    TAGE_SC_L_64KB,
    SimpleBTB,
    SimpleIndirectPredictor,
)


class BTB(SimpleBTB):
    # numEntries = 32*1024
    numEntries = 32 * 1024
    # associativity = 4


class BPLTage(LTAGE):
    instShiftAmt = 0
    requiresBTBHit = True
    takenOnlyHistory = True
    btb = BTB()


# class IT_TAGE(ITTAGE_TAGE):
#     nHistoryTables = 15
#     minHist = 10
#     maxHist = 3881
#     tagTableTagWidths = [0, 9, 9, 13, 13, 13, 13, 13, 13, 13, 13, 15, 15, 15, 15, 15]
#     logTagTableSizes = [12, 11, 11, 9, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8, 8, 8]
#     logUResetPeriod = 19


class BPTageSCL(TAGE_SC_L_64KB):
    instShiftAmt = 0
    requiresBTBHit = True
    # takenOnlyHistory = True
    # indirectBranchPred=ITTAGE(itage=IT_TAGE())
    # indirectBranchPred=ITTAGE()
    # sc_enabled = False
    btb = BTB()


# class BPTageRef(TageSCLRef):
#     instShiftAmt = 2
#     requiresBTBHit = True
#     btb = BTB()


# Set the branch predictor to the BPLTage
cpu.branchPred = BPTageSCL()
# cpu.branchPred = BPTageSCL()
# cpu.branchPred = BPTageRef()
# cpu.branchPred.tage.histBufferSize= 10000
# cpu.branchPred.tage.logTagTableSize= 13
cpu.branchPred.tage.speculativeHistUpdate = True
# cpu.branchPred.tage.speculativeHistUpdate=False
# cpu.branchPred.statistical_corrector.speculativeHistUpdate=True


print(f"Running {args.isa} on {args.cpu_type} CPU: {args.cmd}")


decoupled_FE = True
# decoupled_FE = True
width = 12
if decoupled_FE:
    # We need to configure the decoupled front-end with some specific parameters.
    # First the fetch buffer and fetch target size. We want double the size of
    # the fetch buffer to be able to run ahead of fetch
    cpu.fetchBufferSize = cacheline_size
    cpu.fetchQueueSize = 128
    cpu.fetchTargetWidth = 129
    cpu.minInstSize = 1 if args.isa == "X86" else 4
    cpu.numFTQEntries = 16
    cpu.decoupledFrontEnd = True
    cpu.numPredPerCycle = args.ftNum

    cpu.fetchWidth = width
    cpu.decodeWidth = width
    cpu.renameWidth = width
    cpu.commitWidth = width
    cpu.issueWidth = width
    cpu.wbWidth = width
    cpu.commitWidth = width
    cpu.squashWidth = width
    cpu.dispatchWidth = width


if args.pfc:
    cpu.pfc = True


# The gem5 library simble board which can be used to run simple SE-mode
# simulations.
board = SimpleBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

board.cache_line_size = cacheline_size


# Here we set the workload. In this case we want to run a simple "Hello World!"
# program compiled to the ARM ISA. The `Resource` class will automatically
# download the binary from the gem5 Resources cloud bucket if it's not already
# present.
board.set_se_binary_workload(
    binary=BinaryResource(args.cmd[0]),
    arguments=args.cmd[1:],
)


# Lastly we run the simulation.
simulator = Simulator(board=board)
simulator.run()

print(
    "Exiting @ tick {} because {}.".format(
        simulator.get_current_tick(), simulator.get_last_exit_event_cause()
    )
)
