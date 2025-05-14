# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
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

"""

This script shows an example of running a full system Ubuntu boot simulation
using the gem5 library. This simulation boots Ubuntu 18.04 using 2 KVM CPU
cores. The simulation then switches to 2 Timing CPU cores before running an
echo statement.

Usage
-----

```
scons build/X86/gem5.opt
./build/X86/gem5.opt configs/example/gem5_library/x86-ubuntu-run-with-kvm.py
```
"""

from gem5.utils.requires import requires
from gem5.components.boards.x86_board import X86Board,setPacketRateHack,setAlternatingHack,setNumNics
from gem5.components.memory.single_channel import SingleChannelDDR3_1600
from gem5.components.memory.multi_channel import DualChannelDDR3_1600
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.coherence_protocol import CoherenceProtocol
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent
from gem5.resources.workload import Workload
from gem5.resources.resource import Resource
from gem5.resources.resource import CustomResource
from gem5.resources.resource import DiskImageResource
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)

from gem5.simulate.exit_event_generators import (
    save_checkpoint_generator,
    exit_generator,
)
from pathlib import Path


# This runs a check to ensure the gem5 binary is compiled to X86 and to the
# MESI Two Level coherence protocol.
requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL,
    kvm_required=True,
)

from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)

def envConstruct(is_pci, interrupt_interval,trial):
    mode_str = """export TRIAL="""+str(trial)+""";"""
    pci_str = """export PCI="""+str(is_pci)+""";"""
    interval_str = """export INTERVAL="""+str(interrupt_interval)+""";"""
    return mode_str+pci_str+interval_str
    
def generate_ip_string(num_nics):
    ip_ups =""
    ip_downs =""
    for i in range(num_nics):
        ip_ups += "ip route add default via 192."+str(168-i)+".1.100 dev eth"+str(i)+";\n"
        ip_downs += "ip link set dev eth"+str(i)+" down;\n"
    return ip_ups+ip_downs
    

def generate_bind_unbind_string(num_nics):
    if num_nics > 3:
        nics_enum = list(range(0,4))+ list(range(5,num_nics+1)) #skip 4 IDE controller on 4
    else:
        nics_enum = list(range(0,num_nics))
    unbinds = ""
    binds = ""
    for devid in nics_enum:
        unbinds += "dpdk-devbind.py -u --force 00:0"+str(devid)+".0;\n"
        binds += "dpdk-devbind.py -b uio_pci_generic 00:0"+str(devid)+".0;\n"
    return unbinds+binds
def generate_l3_config(num_nics):
    l3 = "\""
    for i in range(num_nics-1):
        l3+= "("+str(i)+",0,1),"
    l3+= "("+str(num_nics-1)+",0,1),"
    l3+= "\""
    return l3
packet_rate = 0
is_pci = 0
interrupt_interval = 5
mode = 1
num_nics = 1
trial = 10
def main(interrupt_type):
    global packet_rate, is_pci, interrupt_interval,mode

    # Here we setup a MESI Two Level Cache Hierarchy.
    cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
        l1d_size="32kB",
        l1i_size="32kB",
        l2_size="2048kB",
    )
    def ignore_gen():
        while True:
            yield False
    # Setup the system memory.
    memory = DualChannelDDR3_1600(size="16GB")

    # Here we setup the processor. This is a special switchable processor in which
    # a starting core type and a switch core type must be specified. Once a
    # configuration is instantiated a user may call `processor.switch()` to switch
    # from the starting core types to the switch core types. In this simulation
    # we start with KVM cores to simulate the OS boot, then switch to the Timing
    # cores for the command we wish to run after boot.
    processor = SimpleSwitchableProcessor(
        starting_core_type=CPUTypes.KVM,
        switch_core_type=CPUTypes.O3,
        isa=ISA.X86,
        num_cores=2,
        interrupt_type=interrupt_type #"Flush" "Drain" ""
    )
    setPacketRateHack(packet_rate)
    setAlternatingHack(10)
    setNumNics(num_nics)
    # Here we setup the board. The X86Board allows for Full-System X86 simulations.
    board = X86Board(
        clk_freq="2GHz",
        processor=processor,
        memory=memory,
        cache_hierarchy=cache_hierarchy,
        packet_rate=packet_rate
    )

    # Here we set the Full System workload.
    # The `set_kernel_disk_workload` function for the X86Board takes a kernel, a
    # disk image, and, optionally, a command to run.

    # This is the command to run after the system has booted. The first `m5 exit`
    # will stop the simulation so we can switch the CPU cores from KVM to timing
    # and continue the simulation to run the echo command, sleep for a second,
    # then, again, call `m5 exit` to terminate the simulation. After simulation
    # has ended you may inspect `m5out/system.pc.com_1.device` to see the echo
    # output.
    command = (
        "m5 exit;"
        + "echo 'This is running on Timing CPU cores.';"
        + "sleep 1;"
        + "m5 exit;"
    )
    ip = generate_ip_string(num_nics)
    bind = generate_bind_unbind_string(num_nics)
    l3conf = generate_l3_config(num_nics)
    # workload = Workload("x86-ubuntu-18.04-boot")
    board.set_kernel_disk_workload(
        kernel=CustomResource("resources/vmlinux"),
        disk_image=DiskImageResource(
            "ubuntu-x86.img",
            root_partition="1",
        ),
        readfile_contents="""
        #{ sleep 3; m5 exit; } 
        """+ip+"""
        echo 01 > /proc/irq/4/smp_affinity # print only from core 0 core 1 will switch cpu we dont want interrupts there
        echo 01 > /proc/irq/14/smp_affinity # disable ata too.
        modprobe uio_pci_generic
        """+bind+"""
        dpdk-devbind.py -u --force 00:00.0
        
        sleep 3

        dpdk-devbind.py -b uio_pci_generic 00:00.0
        echo 2048 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages;"""+
        envConstruct(is_pci, interrupt_interval, trial)+
        """
        /dpdk/build/examples/dpdk-l3fwd --main-lcore 0 -l 0,1 -- --parse-ptype -p 0xff --config="""+l3conf+"""
        """,
    )
    # workload.set_parameter("readfile_contents", command)
    # board.set_workload(workload)

    simulator = Simulator(
        board=board,
        on_exit_event={
            # using the SimPoints event generator in the standard library to take
            # checkpoints
            ExitEvent.EXIT: exit_generator(),
            ExitEvent.WORKBEGIN: ignore_gen(),
            ExitEvent.WORKEND: ignore_gen(),
            ExitEvent.UTIMER: ignore_gen(),
            ExitEvent.UTIMEREND: ignore_gen()
        },
    )
    simulator.run()
main("Track")