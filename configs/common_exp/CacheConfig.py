# Copyright (c) 2012-2013, 2015-2016 ARM Limited
# Copyright (c) 2020 Barkhausen Institut
# All rights reserved
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
# Copyright (c) 2010 Advanced Micro Devices, Inc.
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

# Configure the M5 cache hierarchy config in one place
#

import m5
from m5.objects import *
from common import ObjectList

from common_exp.Caches import (
    L1_ICache,
    L1_DCache,
    L2Bus,
    L2Cache,
    L3Bus,
    L3Cache,
)


def _get_hwp(hwp_option):
    if hwp_option == None:
        return NULL

    hwpClass = ObjectList.hwp_list.get(hwp_option)
    return hwpClass()


def _get_cache_opts(level, options):
    opts = {}

    size_attr = "{}_size".format(level)
    if hasattr(options, size_attr) and getattr(options, size_attr):
        opts["size"] = getattr(options, size_attr)

    assoc_attr = "{}_assoc".format(level)
    if hasattr(options, assoc_attr) and getattr(options, assoc_attr) > 0:
        opts["assoc"] = getattr(options, assoc_attr)

    prefetcher_attr = "{}_hwp_type".format(level)
    if hasattr(options, prefetcher_attr):
        opts["prefetcher"] = _get_hwp(getattr(options, prefetcher_attr))

    return opts

class BTB(AssociativeBTB):
    numEntries = "8kB"
    assoc = 8

class BPLTage(LTAGE):
    instShiftAmt = 0
    RASSize = 32
    BTB = BTB()
    requiresBTBHit = True

def config_cache(options, system):
    if options.external_memory_system and (options.caches or options.l2cache):
        print("External caches and internal caches are exclusive options.\n")
        sys.exit(1)

    (
        dcache_class,
        icache_class,
        l2_cache_class,
        l3_cache_class,
    ) = (L1_DCache, L1_ICache, L2Cache, L3Cache)

    # Set the cache line size of the system
    system.cache_line_size = options.cacheline_size

    # If elastic trace generation is enabled, make sure the memory system is
    # minimal so that compute delays do not include memory access latencies.
    # Configure the compulsory L1 caches for the O3CPU, do not configure
    # any more caches.
    if options.l2cache and options.elastic_trace_en:
        fatal("When elastic trace is enabled, do not configure L2 caches.")

    num_cpus_per_cluster = options.num_cpus_per_cluster

    for i in range(options.num_cpus):
        if options.caches:
            icache = icache_class(**_get_cache_opts("l1i", options))
            dcache = dcache_class(**_get_cache_opts("l1d", options))

            iwalkcache = None
            dwalkcache = None

            # When connecting the caches, the clock is also inherited
            # from the CPU in question
            system.cpu[i].addPrivateSplitL1Caches(
                icache, dcache, iwalkcache, dwalkcache
            )

            l3_index = int(i / num_cpus_per_cluster)

            assert options.l3cache
            assert options.l2cache

            if i % num_cpus_per_cluster == 0:
                system.cpu[i].l3 = l3_cache_class(
                    **_get_cache_opts("l3", options)
                )
                system.cpu[i].tol3bus = L3Bus(clk_domain=system.cpu_clk_domain)
                system.cpu[i].l3.cpu_side = system.cpu[
                    i
                ].tol3bus.mem_side_ports
                system.cpu[i].l3.mem_side = system.membus.cpu_side_ports

            system.cpu[i].l2 = l2_cache_class(
                clk_domain=system.cpu_clk_domain,
                **_get_cache_opts("l2", options)
            )
            system.cpu[i].tol2bus = L2Bus(clk_domain=system.cpu_clk_domain)
            system.cpu[i].l2.cpu_side = system.cpu[
                i
            ].tol2bus.mem_side_ports
            if options.l3cache:
                l3_index = int(i / num_cpus_per_cluster)
                system.cpu[i].l2.mem_side = system.cpu[
                    l3_index * num_cpus_per_cluster
                ].tol3bus.cpu_side_ports
            else:
                system.cpu[i].l2.mem_side = system.membus.cpu_side_ports

            system.cpu[i].recorder.registerCaches(system.cpu[i].icache, system.cpu[i].l2)
            system.cpu[i].recorder.recorder_port = system.cpu[
                i // num_cpus_per_cluster
            ].tol3bus.cpu_side_ports
            system.cpu[i].branchPred = BPLTage()
            
            if not isinstance(system.cpu[i], X86KvmCPU):
                system.cpu[i].minInstSize = 1
                system.cpu[i].fetchBufferSize = 16
                system.cpu[i].fetchTargetWidth = 32

            if options.decouple: # decoupled frontend
                system.cpu[i].decoupledFrontEnd = True
            if options.fdip:
                system.cpu[i].icache.prefetcher = FetchDirectedPrefetcher(
                    use_virtual_addresses=True, cpu=system.cpu[i]
                )
                system.cpu[i].icache.prefetcher.registerMMU(system.cpu[i].mmu)
            if options.hp:
                system.cpu[i].hp = options.hp
                system.cpu[i].icache.prefetcher = HierarchicalPrefetcher(
                    use_virtual_addresses=True, cpu=system.cpu[i]
                )
                system.cpu[i].icache.prefetcher.registerMMU(system.cpu[i].mmu)
                system.cpu[i].icache.prefetcher.hp_port = system.cpu[
                    i // num_cpus_per_cluster
                ].tol3bus.cpu_side_ports
            if options.eip:
                system.cpu[i].icache.prefetcher = EntanglingPrefetcher(
                    use_virtual_addresses=True, cpu=system.cpu[i]
                )
                system.cpu[i].icache.prefetcher.registerMMU(system.cpu[i].mmu)
                system.cpu[i].icache.prefetcher.listenFromProbeRetiredInstructions(system.cpu[i])


        system.cpu[i].createInterruptController()
        system.cpu[i].connectAllPorts(
            system.cpu[i].tol2bus.cpu_side_ports,
            system.membus.cpu_side_ports,
            system.membus.mem_side_ports,
        )

    return system
