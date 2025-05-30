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

#ifndef __MEM_CACHE_PREFETCH_HP_HH__
#define __MEM_CACHE_PREFETCH_HP_HH__


#include <list>

#include "cpu/base.hh"
#include "cpu/o3/ftq.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/port.hh" // replay
#include "mem/request.hh"
#include "mem/packet.hh"

#include <deque>

namespace gem5
{

struct HierarchicalPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class HierarchicalPrefetcher : public Base
{

  public:
    HierarchicalPrefetcher(const HierarchicalPrefetcherParams &p);
    ~HierarchicalPrefetcher() = default;

    // replay port
    Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;
    // port
    class IcachePort : public RequestPort // port
    {
      private:
        HierarchicalPrefetcher *hp;

      public:
        IcachePort(HierarchicalPrefetcher *hp);
        ~IcachePort() {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override{
            hp->recvTimingResp(pkt);
            return true;
        };
        void recvReqRetry() { hp->recvReqRetry(); }; // seems to not be used
    };
    IcachePort icachePort; // replay

    RequestorID _requestorId = 0;
    uint64_t taskid = 0;

    EventFunctionWrapper tickEvent;
    void tick();

    PacketPtr retryPkt = nullptr;
    bool cacheBlocked = false;

    int max_inflight_read = 50;
    int inflight_read = 0;

    bool isReading = 0; // reading metadata
    std::pair<Addr, Addr> tobeRead {0, 0}; // remaining part
    std::deque<std::pair<Addr, Addr>> rlist;
    std::deque<int> seglist;
    int len_meta; // reading
    void tryRead(Addr paddr);
    void recvReqRetry();
    void recvTimingResp(PacketPtr pkt);

    int spatial_pre = 8;
    int spatial_succ = 24;
    
    uint64_t bytes_read = 0;
    uint64_t read_times = 0;

    void pushSegTiming();
    void startSegTimingReplaying(int _len, std::deque<std::pair<Addr, Addr>> _rlist, std::deque<int> _seglist, uint64_t tid);


    /** Gets a packet from the prefetch queue to be prefetched. */
    PacketPtr getPacket() override;

    Tick nextPrefetchReadyTime() const override
    {
        return pfq.empty() ? MaxTick : pfq.front().readyTime;
    }

    /** Notify functions are not used by this prefetcher. */
    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override {};
    void notifyFill(const PacketPtr &pkt) override{};

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

    /** The prefetch queue entry objects */
    struct PFQEntry
    {
        PFQEntry(uint64_t _addr, PacketPtr p, Tick t)
            : addr(_addr), pkt(p), readyTime(t) {}

        /** The virtual address. Used to scan for redundand prefetches.*/
        uint64_t addr;

        /** The packet that will be sent to the cache. */
        PacketPtr pkt;

        /** The time when the prefetch is ready to be sent to the cache. */
        Tick readyTime;

        bool operator==(const int& a) const {
            return this->addr == a;
        }
    };

    /** The prefetch queue */
    std::list<PFQEntry> pfq;

    /** Creates a prefetch packet for the given address. */
    PacketPtr createPrefetchPacket(Addr addr, bool va=false);

    /** Performs a functional translation of the incomming packet by useing
     * the CPU's TLB. */
    bool translateFunctional(RequestPtr req);


    protected:
      struct Stats : public statistics::Group
      {
          Stats(statistics::Group *parent);
          statistics::Scalar pfInCache;
          statistics::Scalar pfCandidatesAdded;
          statistics::Scalar translationFail;
          statistics::Scalar translationSuccess;
      } stats;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_HP_HH__
