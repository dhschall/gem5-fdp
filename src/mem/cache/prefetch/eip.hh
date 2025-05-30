/*
 * Copyright (c) 2022-2023 The University of Edinburgh
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Implementation of the fetch directed instruction prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_EIP_HH__
#define __MEM_CACHE_PREFETCH_EIP_HH__


#include <map>
#include <set>
#include <list>
#include <deque>
#include <vector>

#include "base/circular_queue.hh"
#include "mem/cache/prefetch/associative_set.hh"

#include "cpu/base.hh"
#include "cpu/o3/ftq.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/request.hh"
#include "mem/packet.hh"

#include <deque>

namespace gem5
{

struct EntanglingPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class EntanglingPrefetcher : public Base
{

  public:

    EntanglingPrefetcher(const EntanglingPrefetcherParams &p);
    ~EntanglingPrefetcher() = default;


    // gets a packet from two prefetch queues to be prefetched.
    PacketPtr getPacket() override;

    Tick nextPrefetchReadyTime() const override
    {
        // return pfq.empty() ? MaxTick : pfq.front().readyTime;
        return pfq.empty() ? MaxTick : pfq.front().readyTime;
    }

    /** The prefetch queue entry objects */
    struct PFQEntry
    {
        PFQEntry(uint64_t _addr, PacketPtr p, Tick t)
            : addr(_addr), pkt(p), readyTime(t) {}

        uint64_t addr;
        PacketPtr pkt;
        Tick readyTime;

        bool operator==(const int& a) const {
            return this->addr == a;
        }
    };

    /** used by eip */
    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override {
        calculatePrefetch(pfi);
    };
    void notifyFill(const PacketPtr &pkt) override{};

    
    std::list<PFQEntry> pfq;

    struct Dst {
        Addr addr = 0;
        int conf = 0;
        uint64_t interval_blks = 0;
        Dst(Addr a, int c, uint64_t b) : addr(a), conf(c), interval_blks(b) {}
    };

    struct BB : public TaggedEntry {
        Addr head_blk = 0; // first blk in BB
        Addr tail_blk = 0; // last blk in BB
        Tick timestamp = 0; // BB's creation time
        uint64_t blk_timestamp = 0; // BB's creation time
        std::deque<Dst> dsts; // (dst-head-blk, confidence)

        BB() : head_blk(0), tail_blk(0), timestamp(0), blk_timestamp(0) {}
        BB(Addr h, Addr s, Tick t, uint64_t b) : head_blk(h), tail_blk(s), timestamp(t), blk_timestamp(b) {}
    };
    AssociativeSet<BB> index; // entangled table
    CircularQueue<BB> buffer; // history buffer

    int get_max_dsts(BB *table_entry, uint64_t src_blk);
    void updateBB(const Addr pc_blk);

    std::deque<std::pair<Addr, Tick>> lat_buffer;
    int lat_buffer_size = 24;

    // frequency
    uint64_t all_blks = 0;
    uint64_t all_insts = 0;
    Addr last_pc_blk = 0;

    // interval cache blocks
    std::deque<Addr> recent_blks;
    uint64_t blk_time = 0;

    // retire sequence
    class PrefetchListenerPC : public ProbeListenerArgBase<Addr>
    {
      public:
        PrefetchListenerPC(EntanglingPrefetcher &_parent, ProbeManager *pm,
                         const std::string &name)
            : ProbeListenerArgBase(pm, name),
              parent(_parent) {}
        void notify(const Addr& pc) override;
      protected:
        EntanglingPrefetcher &parent;
    };
    std::vector<PrefetchListenerPC *> listenersPC;
    void notifyRetiredInst(const Addr pc);

    // icache access sequence
    void calculatePrefetch(const PrefetchInfo &pfi);
    void addEventProbeRetiredInsts(SimObject *obj, const char *name);

  private:

    /** Array of probe listeners */
    std::vector<ProbeListener *> listeners;

    /** Pointer to the CPU object that contains the FTQ */
    BaseCPU *cpu;

    /** For testing purposes */
    const bool transFunctional;

    /** The latency of the prefetcher */
    const unsigned int latency;

    /** Probe the cache before a prefetch gets inserted into the PFQ*/
    const bool cacheSnoop;

    /** Creates a prefetch packet for the given address. */
    PacketPtr createPrefetchPacket(Addr addr, bool va=false);

    /** Performs a functional translation of the incomming packet by useing
     * the CPU's TLB. */
    bool translateFunctional(RequestPtr req);


  protected:
    struct Stats : public statistics::Group
    {
        Stats(statistics::Group *parent);
        statistics::Scalar pfIdentified;
        statistics::Scalar pfInCache;
        statistics::Scalar pfCandidatesAdded;
        statistics::Scalar translationFail;
        statistics::Scalar translationSuccess;
    } stats;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_EIP_HH__
