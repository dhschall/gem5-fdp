# Copyright 2021 Google, Inc.
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

from m5.proxy import Self

from m5.objects.BaseAtomicSimpleCPU import BaseAtomicSimpleCPU
from m5.objects.BaseNonCachingSimpleCPU import BaseNonCachingSimpleCPU
from m5.objects.BaseTimingSimpleCPU import BaseTimingSimpleCPU
from m5.objects.BaseO3CPU import BaseO3CPU
from m5.objects.BaseMinorCPU import BaseMinorCPU
from m5.objects.FuncUnit import *
from m5.objects.FUPool import *
from m5.objects.X86Decoder import X86Decoder
from m5.objects.X86MMU import X86MMU
from m5.objects.X86LocalApic import X86LocalApic
from m5.objects.X86ISA import X86ISA


class X86CPU:
    ArchDecoder = X86Decoder
    ArchMMU = X86MMU
    ArchInterrupts = X86LocalApic
    ArchISA = X86ISA


class X86AtomicSimpleCPU(BaseAtomicSimpleCPU, X86CPU):
    mmu = X86MMU()


class X86NonCachingSimpleCPU(BaseNonCachingSimpleCPU, X86CPU):
    mmu = X86MMU()


class X86TimingSimpleCPU(BaseTimingSimpleCPU, X86CPU):
    mmu = X86MMU()


class X86IntMultDiv(IntMultDiv):
    # DIV and IDIV instructions in x86 are implemented using a loop which
    # issues division microops.  The latency of these microops should really be
    # one (or a small number) cycle each since each of these computes one bit
    # of the quotient.
    opList = [
        OpDesc(opClass="IntMult", opLat=3),
        OpDesc(opClass="IntDiv", opLat=1, pipelined=False),
    ]

    count = 2

class icelake_Simple_Int(FUDesc):
    opList = [ OpDesc(opClass='IntAlu', opLat=1) ]
    count = 4

# Complex ALU instructions
class icelake_Complex_Int(FUDesc):
    opList = [ OpDesc(opClass='IntMult', opLat=4, pipelined=True),
               OpDesc(opClass='IntDiv', opLat=5, pipelined=False) ]
    count = 2

# FP/AVX instructions
class icelake_FP(FUDesc):
    opList = [ OpDesc(opClass='SimdAdd', opLat=1),
               OpDesc(opClass='SimdAddAcc', opLat=1),
               OpDesc(opClass='SimdAlu', opLat=1),
               OpDesc(opClass='SimdCmp', opLat=1),
               OpDesc(opClass='SimdCvt', opLat=1),
               OpDesc(opClass='SimdMisc', opLat=3),
               OpDesc(opClass='SimdMult',opLat=5),
               OpDesc(opClass='SimdMultAcc',opLat=5),
               OpDesc(opClass='SimdShift',opLat=4),
               OpDesc(opClass='SimdShiftAcc', opLat=1),
               OpDesc(opClass='SimdSqrt', opLat=5),
               OpDesc(opClass='SimdFloatAdd',opLat=1),
               OpDesc(opClass='SimdFloatAlu',opLat=1),
               OpDesc(opClass='SimdFloatCmp', opLat=1),
               OpDesc(opClass='SimdFloatCvt', opLat=3),
               OpDesc(opClass='SimdFloatDiv', opLat=3),
               OpDesc(opClass='SimdFloatMisc', opLat=3),
               OpDesc(opClass='SimdFloatMult', opLat=3),
               OpDesc(opClass='SimdFloatMultAcc',opLat=1),
               OpDesc(opClass='SimdFloatSqrt', opLat=9),
               OpDesc(opClass='FloatAdd', opLat=3),
               OpDesc(opClass='FloatCmp', opLat=3),
               OpDesc(opClass='FloatCvt', opLat=3),
               OpDesc(opClass='FloatDiv', opLat=9, pipelined=False),
               OpDesc(opClass='FloatSqrt', opLat=33, pipelined=False),
               OpDesc(opClass='FloatMult', opLat=4),
               OpDesc(opClass='FloatMultAcc', opLat=5),
               OpDesc(opClass='FloatMisc', opLat=3) ]
    count = 3

# Load Units
class icelake_Load(FUDesc):
    opList = [ OpDesc(opClass='MemRead',opLat=4),
               OpDesc(opClass='FloatMemRead',opLat=4) ]
    count = 2

# Store Units
class icelake_Store(FUDesc):
    opList = [ OpDesc(opClass='MemWrite',opLat=2),
               OpDesc(opClass='FloatMemWrite',opLat=2) ]
    count = 4

# Functional Units
class icelake_FUP(FUPool):
    FUList = [icelake_Simple_Int(), icelake_Complex_Int(),
              icelake_Load(), icelake_Store(), icelake_FP()]
class DefaultX86FUPool(FUPool):
    FUList = [
        IntALU(),
        X86IntMultDiv(),
        FP_ALU(),
        FP_MultDiv(),
        ReadPort(),
        SIMD_Unit(),
        PredALU(),
        WritePort(),
        RdWrPort(),
        IprPort(),
    ]


class X86O3CPU(BaseO3CPU, X86CPU):
    mmu = X86MMU()
    needsTSO = True
    LQEntries = 128
    SQEntries = 72
    LSQDepCheckShift = 0
    LFSTSize = 1024
    SSITSize = 1024
    decodeToFetchDelay = 1
    renameToFetchDelay = 1
    iewToFetchDelay = 1
    commitToFetchDelay = 1
    renameToDecodeDelay = 1
    iewToDecodeDelay = 1
    commitToDecodeDelay = 1
    iewToRenameDelay = 1
    commitToRenameDelay = 1
    commitToIEWDelay = 1
    fetchWidth = 6
    fetchBufferSize = 16
    fetchQueueSize = 70
    fetchToDecodeDelay = 3
    decodeWidth = 6
    decodeToRenameDelay = 2
    renameWidth = 10
    renameToIEWDelay = 1
    issueToExecuteDelay = 1
    dispatchWidth = 10
    issueWidth = 10
    wbWidth = 10
    iewToCommitDelay = 1
    renameToROBDelay = 1
    commitWidth = 10
    squashWidth = 10
    trapLatency = 13
    backComSize = 10
    forwardComSize = 10
    numPhysIntRegs = 988 + (39*1)
    numPhysFloatRegs = 288 + (48*1)
    numPhysVecRegs = 288  + (48*1)
    numIQEntries = 168
    numROBEntries = 384


    # For x86, each CC reg is used to hold only a subset of the
    # flags, so we need 4-5 times the number of CC regs as
    # physical integer regs to be sure we don't run out.  In
    # typical real machines, CC regs are not explicitly renamed
    # (it's a side effect of int reg renaming), so they should
    # never be the bottleneck here.
    numPhysCCRegs = (988 + (39*1)) * 5

    # DIV and IDIV instructions in x86 are implemented using a loop which
    # issues division microops.  The latency of these microops should really be
    # one (or a small number) cycle each since each of these computes one bit
    # of the quotient.
    fuPool = icelake_FUP()

class X86O3CPUDrain(X86O3CPU):
    intStrategy = Param.InterruptStrategy(
        "Drain", "Interrupt Cleanup Strategy"
    )

class X86O3CPUFlush(X86O3CPU):
    intStrategy = Param.InterruptStrategy(
        "Flush", "Interrupt Cleanup Strategy"
    )

class X86O3CPUTrack(X86O3CPU):
    intStrategy = Param.InterruptStrategy(
        "Intelligent", "Interrupt Cleanup Strategy"
    )

class X86O3CPUApic(X86O3CPU):
    intStrategy = Param.InterruptStrategy(
        "Apic", "Interrupt Cleanup Strategy"
    )
class X86MinorCPU(BaseMinorCPU, X86CPU):
    mmu = X86MMU()
