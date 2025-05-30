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


#include "mem/cache/prefetch/hp.hh"

#include <utility>

#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/HierarchicalPrefetcher.hh"
#include "debug/CountTask.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

HierarchicalPrefetcher::IcachePort::IcachePort(HierarchicalPrefetcher *_hp)
    : RequestPort("hp.iport"), hp(_hp)
{}

HierarchicalPrefetcher::HierarchicalPrefetcher(
                                const HierarchicalPrefetcherParams &p)
    : Base(p), icachePort(this),
      _requestorId(p.sys->getRequestorId(this)),
      tickEvent([this]{ tick(); }, "replayer tick", false),
      cpu(p.cpu),
      transFunctional(p.translate_functional),
      latency(cyclesToTicks(p.latency)),
      cacheSnoop(true), stats(this)
{
    cpu->hwp = this; // register hwp
    cpu->recorder->hwp = this; // register hwp
}

Port & HierarchicalPrefetcher::getPort(const std::string &if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "This object doesn't support vector ports");
    if (if_name == "hp_port") { return icachePort; }
	else { return SimObject::getPort(if_name, idx); }
}


void HierarchicalPrefetcher::tick(){    
    if (inflight_read < max_inflight_read)
        tryRead(0);

    if (!tickEvent.scheduled() && len_meta > 0)
        schedule(tickEvent, clockEdge(Cycles(1)));
}

void HierarchicalPrefetcher::tryRead(Addr paddr){
    if(cacheBlocked) {
        return;
    }

    if (len_meta == 0) {
        DPRINTF(CountTask, "no region toberead\n");
        return;
    }
    assert(len_meta > 0);
    assert(rlist.size() > 0);
    assert(rlist[0].first < rlist[0].second);

    Addr meta_addr = rlist[0].first; // meta_addr
    int meta_size = blkSize; // 64
    if (meta_size > rlist[0].second - rlist[0].first) meta_size = rlist[0].second - rlist[0].first;
    if (meta_size > len_meta * 16) meta_size = len_meta * 16; // meta_size
    uint8_t *meta_data = new uint8_t[meta_size]; // meta_data

    // build request
    assert(_requestorId != 0);
    RequestPtr req = std::make_shared<Request>(
        meta_addr, meta_size, Request::PHYSICAL, _requestorId);
    assert(req->hasPaddr());

    // build packet
    PacketPtr data_pkt = new Packet(req, MemCmd::ReadReq);
    data_pkt->dataDynamic(meta_data);

    if(!icachePort.sendTimingReq(data_pkt)){
        // blocked
        assert(retryPkt == nullptr);
        retryPkt = data_pkt;
        cacheBlocked = true;
    } else {
        bytes_read += meta_size;
        read_times++;
        if (read_times % 1000 == 0)
            DPRINTF(CountTask, "bytes read=%lu\n", bytes_read);

        inflight_read++;
        // sent successed
        assert(meta_size % 16 == 0);
        len_meta -= meta_size / 16;
        rlist[0].first += meta_size;
        if (rlist[0].first == rlist[0].second) rlist.pop_front();
        if (seglist.size() == 0 && len_meta == 0) {
            DPRINTF(CountTask, "task[%#lx] metadata readreqs all sent\n", taskid);
            len_meta = 0; // redundant but clear
            rlist.clear();
            taskid = 0;
        }
    }
}

void HierarchicalPrefetcher::recvReqRetry(){
    if (icachePort.sendTimingReq(retryPkt)) {
        inflight_read++;
        retryPkt = nullptr;
        cacheBlocked = false;
    } else {
        DPRINTF(CountTask, "replayer recvreqretry failed\n");
    }
}

void HierarchicalPrefetcher::recvTimingResp(PacketPtr pkt){
    inflight_read--;
    assert(pkt->getSize() % 16 == 0);
    int spatialregion_num = pkt->getSize() / 16;

    uint8_t buffer[64]; // upper limit
    memcpy(buffer, pkt->getConstPtr<uint8_t>(), pkt->getSize());

    std::deque<Addr> ptrace;
    // decrypt addresses (to prefetch)
    for(int i = 0; i < spatialregion_num; i++){ // 16 bytes, addr, pre, succ
        int base_index = i * 16;

        // get trigger_addr blk
        uint64_t trigger_blk = 0;
        for (int j = 0; j < 8; j++)
            trigger_blk |= (uint64_t)buffer[base_index + j] << j*8;

        // get prec and succ
        std::vector<bool> prec;
        std::vector<bool> succ;
        prec.resize(spatial_pre, false);
        succ.resize(spatial_succ, false);

        base_index += 8; // start of pre and succ
        for (int i = 0; i < spatial_pre; i += 8) {
            uint8_t byte = buffer[base_index++];
            for (int j = 0; j < 8 && (i+j) < spatial_pre; j++) {
                prec[i+j] = (byte >> j) & 1;
            }
        }
        for (int i = 0; i < spatial_succ; i += 8) {
            uint8_t byte = buffer[base_index++];
            for (int j = 0; j < 8 && (i+j) < spatial_succ; j++) {
                succ[i+j] = (byte >> j) & 1;
            }
        }

        // push prec
        for (int j = prec.size() - 1; j >= 0; j--) {
            if (prec[j]) {
                Addr addr_blk = trigger_blk - (j + 1);
                ptrace.push_back(addr_blk << lBlkSize);
            }
        }
        // push trigger
        ptrace.push_back(trigger_blk << lBlkSize);
        // push succ
        for (int j = 0; j < succ.size(); j++) {
            if (succ[j]) {
                Addr addr_blk = trigger_blk + (j + 1);
                ptrace.push_back(addr_blk << lBlkSize);
            }
        }
    }
    
    // push ptrace to pfq (vtrace)
    for (int i = 0; i < ptrace.size(); i++) {
        Addr paddr = ptrace[i];

        // Create the packet for this address
        PacketPtr pkt = createPrefetchPacket(paddr, false);
        if (!pkt) continue;

        if (cacheSnoop && (inCache(pkt->getAddr(), pkt->isSecure())
                    || (inMissQueue(pkt->getAddr(), pkt->isSecure())))) {
            stats.pfInCache++;
            delete pkt;
            continue;
        }

        Tick t = curTick() + latency;
        stats.pfCandidatesAdded++;
        pfq.push_back(PFQEntry(paddr, pkt, t));
    }
}

void HierarchicalPrefetcher::pushSegTiming() {
    assert(seglist.size() > 0);
    len_meta += seglist[0];
    seglist.pop_front();

    if(!tickEvent.scheduled())
        schedule(tickEvent, clockEdge(Cycles(0)));
}

void HierarchicalPrefetcher::startSegTimingReplaying(int _len, std::deque<std::pair<Addr, Addr>> _rlist, std::deque<int> _seglist, uint64_t tid) {
    pfq.clear();

    // read by segments
    len_meta = 0;
    taskid = tid;
    rlist = _rlist;
    seglist = _seglist;
}




PacketPtr
HierarchicalPrefetcher::createPrefetchPacket(Addr addr, bool virtual_addr)
{
    /* Create a prefetch memory request */
    RequestPtr req = nullptr;
    Flags flags = Request::INST_FETCH|Request::PREFETCH;

    if (virtual_addr) {
        // The address is virtual -> we need translate first
        req = std::make_shared<Request>(
                addr, blkSize, flags, requestorId, addr, 0);


        // Translate the address from virtual to physical using
        // the functional translation function.
        // Note: This of course a hack and is not how it is in a real system.
        // Using functional tranlation underestimate the latency of the
        // translation and the page walk.
        // TODO: Add timing translation!
        if (!translateFunctional(req)) {
            return nullptr;
        }

    } else {
        // The adddress is physical -> no translation needed.
        req = std::make_shared<Request>(
                addr, blkSize, flags, requestorId);
    }

    req->taskId(context_switch_task_id::Prefetcher);
    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();

    return pkt;
}


bool
HierarchicalPrefetcher::translateFunctional(RequestPtr req)
{
    if (mmu == nullptr) {
        return false;
    }

    auto tc = cache->system->threads[req->contextId()];

    DPRINTF(HWPrefetch, "%s Try trans of pc %#x\n",
                                mmu->name(), req->getVaddr());
    Fault fault = mmu->translateFunctional(req, tc, BaseMMU::Read);
    if (fault == NoFault) {
        DPRINTF(HWPrefetch, "%s Translation of vaddr %#x succeeded: "
                        "paddr %#x \n", mmu->name(), req->getVaddr(),
                        req->getPaddr());

        stats.translationSuccess++;
        return true;
    }
    stats.translationFail++;
    return false;
}


PacketPtr
HierarchicalPrefetcher::getPacket()
{
    if (pfq.size() == 0) {
        return nullptr;
    }
    PacketPtr pkt = pfq.front().pkt;

    DPRINTF(HWPrefetch, "Issue Prefetch to: pkt:%#x, PC:%#x, PFQ size:%i\n",
                        pkt->getAddr(), pfq.front().addr, pfq.size());

    pfq.pop_front();

    prefetchStats.pfIssued++;
    issuedPrefetches++;
    return pkt;
}


HierarchicalPrefetcher::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(pfInCache, statistics::units::Count::get(),
            "number of prefetches hit in in cache"),
    ADD_STAT(pfCandidatesAdded, statistics::units::Count::get(),
            "Number of perfetch candidates added to the prefetch queue"),
    ADD_STAT(translationFail, statistics::units::Count::get(),
             "Number of prefetches that failed translation"),
    ADD_STAT(translationSuccess, statistics::units::Count::get(),
             "Number of prefetches that succeeded translation")
{
}

} // namespace prefetch
} // namespace gem5
