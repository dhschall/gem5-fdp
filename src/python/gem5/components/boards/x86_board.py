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


from .kernel_disk_workload import KernelDiskWorkload
from ...resources.resource import AbstractResource
from ...utils.override import overrides
from .abstract_system_board import AbstractSystemBoard
from ...isas import ISA

from m5.objects import (
    Pc,
    AddrRange,
    X86FsLinux,
    Addr,
    X86SMBiosBiosInformation,
    X86IntelMPProcessor,
    X86IntelMPIOAPIC,
    X86IntelMPBus,
    X86IntelMPBusHierarchy,
    X86IntelMPIOIntAssignment,
    X86E820Entry,
    Bridge,
    IOXBar,
    IdeDisk,
    CowDiskImage,
    RawDiskImage,
    BaseXBar,
    Port,
    IGbE_e1000,
    EtherLink,
    CopyEngine,
    LoadGenerator,
    Accelerator
)

from m5.util.convert import toMemorySize
from m5.util import *

from ..processors.abstract_processor import AbstractProcessor
from ..memory.abstract_memory_system import AbstractMemorySystem
from ..cachehierarchies.abstract_cache_hierarchy import AbstractCacheHierarchy

from typing import List, Sequence
class PacketRate:
    packet_rate_in_Gbps = 6.0
class AlternatingRate:
    alternating_rate = 10
class IsAccel:
    accelerator = False
class Vmr:
    vmr = 1
class MaxError:
    max_error = 1
class NumNics:
    num_nics = 1
#this is for the attribute error 
def getPacketRateHack():
    return PacketRate.packet_rate_in_Gbps
def setPacketRateHack(val):
    PacketRate.packet_rate_in_Gbps = val
def getAlternatingHack():
    return AlternatingRate.alternating_rate
def setAlternatingHack(val):
    AlternatingRate.alternating_rate = val
def getIsAccelHack():
    return IsAccel.accelerator
def setIsAccelHack(val):
    IsAccel.accelerator = val
def getVmrHack():
    return Vmr.vmr
def setVmrHack(val):
    Vmr.vmr = val
def getMaxErrorHack():
    return MaxError.max_error
def setMaxErrorHack(val):
    MaxError.max_error = val
def setNumNics(val):
    NumNics.num_nics = val
def getNumNics():
    return NumNics.num_nics
class X86Board(AbstractSystemBoard, KernelDiskWorkload):
    """
    A board capable of full system simulation for X86.

    **Limitations**
    * Currently, this board's memory is hardcoded to 3GB
    * Much of the I/O subsystem is hard coded
    """
    
    def __init__(
        self,
        clk_freq: str,
        processor: AbstractProcessor,
        memory: AbstractMemorySystem,
        cache_hierarchy: AbstractCacheHierarchy,
        packet_rate: float
    ) -> None:
        super().__init__(
            clk_freq=clk_freq,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
        )
        setPacketRateHack(packet_rate)
        if self.get_processor().get_isa() != ISA.X86:
            raise Exception(
                "The X86Board requires a processor using the X86 "
                f"ISA. Current processor ISA: '{processor.get_isa().name}'."
            )

    @overrides(AbstractSystemBoard)
    def _setup_board(self) -> None:
        print("PACKET RATE: " +str(getPacketRateHack()))
        print("Alternation Rate: "+str(getAlternatingHack()))
        num_nics = getNumNics()
        nics = []
        loadgens = []
        ethifs = []
        if num_nics > 3:
            nics_enum = list(range(0,4))+ list(range(5,num_nics+1)) #skip 4 IDE controller on 4
        else:
            nics_enum = list(range(0,num_nics))
        if getIsAccelHack():
            for i,devid in enumerate(nics_enum):
                loadgens.append(Accelerator(accel_id = devid, packet_size = 1514, vmr=getVmrHack(),max_error=getMaxErrorHack(),mode='Normal'))
        else:
            for i,devid in enumerate(nics_enum):
                loadgens.append(LoadGenerator(loadgen_id = devid,packet_rate = getPacketRateHack()*88650, packet_size = 1514, start_tick=30097127313142, stop_tick=45000000000000000,burst_gap=getAlternatingHack(),burst_width=500*100000 ,mode='Poisson'))
        ioat = CopyEngine(pci_bus=0,pci_dev=num_nics+1,pci_func=0)
        
        for i,devid in enumerate(nics_enum):
            # nics.append(IGbE_e1000(pci_bus=0, pci_dev=i, pci_func=0,
                            #  InterruptLine=(16+i), InterruptPin=1))
            nics.append(IGbE_e1000(adq_idx=devid,pci_bus=0, pci_dev=devid, pci_func=0,
                              InterruptLine=(16+devid), InterruptPin=1))
            ethifs.append(EtherLink(speed = '1000Gbps'))
            ethifs[i].int0 = nics[i].interface
            ethifs[i].int1 = loadgens[i].interface
        self.ethifs = ethifs
        self.loadgens = loadgens
        class ModifiedPc(Pc):
            def __init__(self,nics,ioat):
                super(ModifiedPc, self).__init__()
                self.nics = nics
                self.ioat = ioat


            def attachIO(self, bus, dma_ports = []):
                super(ModifiedPc, self).attachIO(bus, dma_ports)
                for dev in self.nics:
                    dev.host = self.pci_host
                    dev.dma = bus.cpu_side_ports
                    dev.pio = bus.mem_side_ports
                #print("DMA CONNECTING")
                self.ioat.host = self.pci_host
                for i in range(4):
                   self.ioat.dma_local[i] = bus.cpu_side_ports
                self.ioat.dma = bus.cpu_side_ports
                self.ioat.pio = bus.mem_side_ports
                    

        self.pc = ModifiedPc(nics,ioat)

        self.workload = X86FsLinux()
        
        # North Bridge
        self.iobus = IOXBar()

        # Set up all of the I/O.
        self._setup_io_devices()

        self.m5ops_base = 0xFFFF0000

    def _setup_io_devices(self):
        """Sets up the x86 IO devices.

        Note: This is mostly copy-paste from prior X86 FS setups. Some of it
        may not be documented and there may be bugs.
        """

        # Constants similar to x86_traits.hh
        IO_address_space_base = 0x8000000000000000
        pci_config_address_space_base = 0xC000000000000000
        interrupts_address_space_base = 0xA000000000000000
        APIC_range_size = 1 << 12

        # Setup memory system specific settings.
        if self.get_cache_hierarchy().is_ruby():
            self.pc.attachIO(self.get_io_bus(), [self.pc.south_bridge.ide.dma])
        else:
            self.bridge = Bridge(delay="50ns")
            self.bridge.mem_side_port = self.get_io_bus().cpu_side_ports
            self.bridge.cpu_side_port = (
                self.get_cache_hierarchy().get_mem_side_port()
            )

            # # Constants similar to x86_traits.hh
            IO_address_space_base = 0x8000000000000000
            pci_config_address_space_base = 0xC000000000000000
            interrupts_address_space_base = 0xA000000000000000
            APIC_range_size = 1 << 12

            self.bridge.ranges = [
                AddrRange(0xC0000000, 0xFFFF0000),
                AddrRange(
                    IO_address_space_base, interrupts_address_space_base - 1
                ),
                AddrRange(pci_config_address_space_base, Addr.max),
            ]

            self.apicbridge = Bridge(delay="50ns")
            self.apicbridge.cpu_side_port = self.get_io_bus().mem_side_ports
            self.apicbridge.mem_side_port = (
                self.get_cache_hierarchy().get_cpu_side_port()
            )
            self.apicbridge.ranges = [
                AddrRange(
                    interrupts_address_space_base,
                    interrupts_address_space_base
                    + self.get_processor().get_num_cores() * APIC_range_size
                    - 1,
                )
            ]
            self.pc.attachIO(self.get_io_bus())

        # Add in a Bios information structure.
        self.workload.smbios_table.structures = [X86SMBiosBiosInformation()]
        
        # Set up the Intel MP table
        base_entries = []
        ext_entries = []
        for i in range(self.get_processor().get_num_cores()):
            bp = X86IntelMPProcessor(
                local_apic_id=i,
                local_apic_version=0x14,
                enable=True,
                bootstrap=(i == 0),
            )
            base_entries.append(bp)

        io_apic = X86IntelMPIOAPIC(
            id=self.get_processor().get_num_cores(),
            version=0x11,
            enable=True,
            address=0xFEC00000,
        )

        self.pc.south_bridge.io_apic.apic_id = io_apic.id
        base_entries.append(io_apic)
        pci_bus = X86IntelMPBus(bus_id=0, bus_type="PCI   ")
        base_entries.append(pci_bus)
        isa_bus = X86IntelMPBus(bus_id=1, bus_type="ISA   ")
        base_entries.append(isa_bus)
        connect_busses = X86IntelMPBusHierarchy(
            bus_id=1, subtractive_decode=True, parent_bus=0
        )
        ext_entries.append(connect_busses)

        for dev in range(0, 10):
            pci_dev_inta = X86IntelMPIOIntAssignment(
                interrupt_type="INT",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=0,
                source_bus_irq=0 + (dev << 2),
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=16 + dev,
            )
            base_entries.append(pci_dev_inta)

        def assignISAInt(irq, apicPin):

            assign_8259_to_apic = X86IntelMPIOIntAssignment(
                interrupt_type="ExtInt",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=0,
            )
            base_entries.append(assign_8259_to_apic)

            assign_to_apic = X86IntelMPIOIntAssignment(
                interrupt_type="INT",
                polarity="ConformPolarity",
                trigger="ConformTrigger",
                source_bus_id=1,
                source_bus_irq=irq,
                dest_io_apic_id=io_apic.id,
                dest_io_apic_intin=apicPin,
            )
            base_entries.append(assign_to_apic)

        assignISAInt(0, 2)
        assignISAInt(1, 1)

        for i in range(3, 15):
            assignISAInt(i, i)

        self.workload.intel_mp_table.base_entries = base_entries
        self.workload.intel_mp_table.ext_entries = ext_entries

        entries = [
            # Mark the first megabyte of memory as reserved
            X86E820Entry(addr=0, size="639kB", range_type=1),
            X86E820Entry(addr=0x9FC00, size="385kB", range_type=2),
            # Mark the rest of physical memory as available
            X86E820Entry(
                addr=0x100000,
                size=f"{self.mem_ranges[0].size() - 0x100000:d}B",
                range_type=1,
            ),
        ]

        # Reserve the last 16kB of the 32-bit address space for m5ops
        entries.append(
            X86E820Entry(addr=0xFFFF0000, size="64kB", range_type=2)
        )
        if len(self.mem_ranges) == 3:
            print("x")
            entries.append(
                X86E820Entry(
                    addr=0x100000000,
                    size="%dB" % (self.mem_ranges[1].size()),
                    range_type=1,
                )
            )
        self.workload.e820_table.entries = entries

    @overrides(AbstractSystemBoard)
    def has_io_bus(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_io_bus(self) -> BaseXBar:
        return self.iobus

    @overrides(AbstractSystemBoard)
    def has_dma_ports(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_dma_ports(self) -> Sequence[Port]:
        return [self.pc.south_bridge.ide.dma, self.iobus.mem_side_ports]

    @overrides(AbstractSystemBoard)
    def has_coherent_io(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_mem_side_coherent_io_port(self) -> Port:
        return self.iobus.mem_side_ports

    @overrides(AbstractSystemBoard)
    def _setup_memory_ranges(self):
        memory = self.get_memory()

        excess_mem_size = memory.get_size() - toMemorySize("3GB")
        if excess_mem_size <= 0:
            data_range = AddrRange(memory.get_size())
            memory.set_memory_range([data_range])

            # Add the address range for the IO
            self.mem_ranges = [
                data_range,  # All data
                AddrRange(0xC0000000, size=0x100000),  # For I/0
            ]
        else:
            warn(
                "Physical memory size specified is %s which is greater than "
                "3GB.  Twice the number of memory controllers would be "
                "created." % (memory.get_size())
            )

            self.mem_ranges = [
                AddrRange("3GB"),
                AddrRange(Addr("4GB"), size=excess_mem_size),
                AddrRange(0xC0000000, size=0x100000),  # For I/0
            ]
            memory.set_memory_range([AddrRange("3GB"), AddrRange(Addr("4GB"), size=excess_mem_size)])


        # if memory.get_size() > toMemorySize("3GB"):
            # raise Exception(
                # "X86Board currently only supports memory sizes up "
                # "to 3GB because of the I/O hole."
            # )

        

    @overrides(KernelDiskWorkload)
    def get_disk_device(self):
        return "/dev/sda"

    @overrides(KernelDiskWorkload)
    def _add_disk_to_board(self, disk_image: AbstractResource):
        ide_disk = IdeDisk()
        ide_disk.driveID = "device0"
        ide_disk.image = CowDiskImage(
            child=RawDiskImage(read_only=True), read_only=False
        )
        ide_disk.image.child.image_file = disk_image.get_local_path()

        # Attach the SimObject to the system.
        self.pc.south_bridge.ide.disks = [ide_disk]

    @overrides(KernelDiskWorkload)
    def get_default_kernel_args(self) -> List[str]:
        return [
            "earlyprintk=ttyS0",
            "console=ttyS0",
            "root={root_value}",
            "intel_idle. max_cstate=0",
            "idle=nomwait",
            "lpj=7999923",
            "notsc",
        ]
