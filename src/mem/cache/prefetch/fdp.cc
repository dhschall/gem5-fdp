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


#include "mem/cache/prefetch/fdp.hh"

#include <utility>

#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/FetchDirectedPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FetchDirectedPrefetcher::FetchDirectedPrefetcher(
                                const FetchDirectedPrefetcherParams &p)
    : Base(p),
      cpu(p.cpu),
      cache(nullptr),
      transFunctional(p.translate_functional),
      latency(cyclesToTicks(p.latency)), cacheSnoop(true),
      stats(this)
{
}


void
FetchDirectedPrefetcher::notifyFTQInsert(const o3::FetchTargetPtr& ft)
{
    Addr blkAddr = blockAddress(ft->startAddress());
    notifyPfAddr(blkAddr, true);
}


void
FetchDirectedPrefetcher::notifyFTQRemove(const o3::FetchTargetPtr& ft)
{
}


void
FetchDirectedPrefetcher::notifyPfAddr(Addr addr, bool virtual_addr)
{
    Addr blk_addr = blockAddress(addr);

    // Check if the address is already in the prefetch queue
    std::list<PrefetchRequest>::iterator it = std::find(pfq.begin(),
                                pfq.end(), blk_addr);
    if (it != pfq.end()) {
        DPRINTF(HWPrefetch, "%#x already in prefetch_queue\n", blk_addr);
        return;
    }

    it = std::find(translationq.begin(), translationq.end(), blk_addr);
    if (it != translationq.end()) {
        DPRINTF(HWPrefetch, "%#x already in translation queue\n", blk_addr);
        return;
    }

    stats.pfIdentified++;

    translationq.emplace_back(*this, blk_addr);
    translationq.back().startTranslation();

}




RequestPtr
FetchDirectedPrefetcher::createPrefetchRequest(Addr vaddr)
{
    RequestPtr req = std::make_shared<Request>(
            vaddr, blkSize, 0, requestorId, vaddr, 0);
    req->setFlags(Request::PREFETCH);
    return req;
}


PacketPtr
FetchDirectedPrefetcher::createPrefetchPacket(Addr addr, bool virtual_addr)
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

    if (req->isUncacheable()) {
        return nullptr;
    }

    req->taskId(context_switch_task_id::Prefetcher);
    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();

    return pkt;
}


void
FetchDirectedPrefetcher::translationComplete(PrefetchRequest *pfr, bool failed)
{
    auto it = translationq.begin();
    while (it != translationq.end()) {
        if (&(*it) == pfr) {
            break;
        }
        ++it;
    }
    assert(it != translationq.end());
    assert(cache != nullptr);

    if (failed) {
        DPRINTF(HWPrefetch, "Translation of %#x failed\n", it->addr);
        stats.translationFail++;
    } else {
        DPRINTF(HWPrefetch, "Translation of %#x succeeded\n", it->addr);
        stats.translationSuccess++;

        if (cacheSnoop &&
            (cache->inCache(it->req->getPaddr(), it->req->isSecure())
         || (cache->inMissQueue(it->req->getPaddr(), it->req->isSecure())))) {
            stats.pfInCache++;
            DPRINTF(HWPrefetch, "Drop Packet. In Cache / MSHR\n");
        } else {

            it->createPkt(curTick() + latency);
            stats.pfPacketsCreated++;
            DPRINTF(HWPrefetch, "Addr: %#x Add packet to PFQ. pkt PA:%#x, "
                    "PFQ sz:%i\n", it->addr, it->pkt->getAddr(), pfq.size());

            stats.pfCandidatesAdded++;
            pfq.push_back(*it);
        }
    }
    translationq.erase(it);
}



bool
FetchDirectedPrefetcher::translateFunctional(RequestPtr req)
{
    if (mmu == nullptr) {
        return false;
    }

    auto tc = system->threads[req->contextId()];

    DPRINTF(HWPrefetch, "%s Try trans of pc %#x\n",
                                mmu->name(), req->getVaddr());
    Fault fault = mmu->translateFunctional(req, tc, BaseMMU::Execute);
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
FetchDirectedPrefetcher::getPacket()
{
    if (pfq.size() == 0)
    {
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

FetchDirectedPrefetcher::PrefetchRequest::PrefetchRequest(
    FetchDirectedPrefetcher& _owner, uint64_t _addr)
    : owner(_owner),
      addr(_addr),
      req(nullptr),
      pkt(nullptr)
{
    createRequest();
    assert(req);
}

void
FetchDirectedPrefetcher::PrefetchRequest::createRequest()
{
    Flags flags = Request::INST_FETCH|Request::PREFETCH;
    req = std::make_shared<Request>(
            addr, owner.blkSize, flags, owner.requestorId, addr, 0);
    req->setFlags(Request::PREFETCH);
}

void
FetchDirectedPrefetcher::PrefetchRequest::createPkt(Tick t)
{
    req->taskId(context_switch_task_id::Prefetcher);
    pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
    readyTime = t;
}

void
FetchDirectedPrefetcher::PrefetchRequest::startTranslation()
{
    assert(owner.mmu != nullptr);
    auto tc = owner.system->threads[req->contextId()];
    // DPRINTF(HWPrefetch, "%s Try translation of pc %#x\n",
    //     owner.mmu->name(), req->getVaddr());
    owner.mmu->translateTiming(req, tc, this, BaseMMU::Execute);
}

void
FetchDirectedPrefetcher::PrefetchRequest::finish(const Fault &fault,
    const RequestPtr &req, ThreadContext *tc, BaseMMU::Mode mode)
{
    bool failed = (fault != NoFault);
    owner.translationComplete(this, failed);
}

void
FetchDirectedPrefetcher::regProbeListeners()
{
    Base::regProbeListeners();

    if (cpu == nullptr) {
        warn("No CPU to listen from registered\n");
        return;
    }
    typedef ProbeListenerArgFunc<o3::FetchTargetPtr> FetchTargetListener;
    listeners.push_back(
            cpu->getProbeManager()->connect<FetchTargetListener>("FTQInsert",
                [this](const o3::FetchTargetPtr &ft)
                    { notifyFTQInsert(ft); }));

    listeners.push_back(
            cpu->getProbeManager()->connect<FetchTargetListener>("FTQRemove",
                [this](const o3::FetchTargetPtr &ft)
                    { notifyFTQRemove(ft); }));

}


FetchDirectedPrefetcher::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(fdipInsertions, statistics::units::Count::get(),
            "Number of notifications from an insertion in the FTQ"),
    ADD_STAT(pfIdentified, statistics::units::Count::get(),
            "number of prefetches identified."),
    ADD_STAT(pfInCache, statistics::units::Count::get(),
            "number of prefetches hit in in cache"),
    ADD_STAT(pfInCachePrefetched, statistics::units::Count::get(),
            "number of prefetches hit in cache but prefetched"),
    ADD_STAT(pfPacketsCreated, statistics::units::Count::get(),
            "number of prefetch packets created"),
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
