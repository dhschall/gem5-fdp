/*
 * Copyright (c) 2010-2014 ARM Limited
 * Copyright (c) 2012-2013 AMD
 * Copyright (c) 2022-2023 The University of Edinburgh
 * All rights reserved.
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
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

#include "cpu/o3/fetch.hh"

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <queue>

#include "arch/generic/tlb.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "cpu/exetrace.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port"), fetch(_fetch)
{}


Fetch::Fetch(CPU *_cpu, const BaseO3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      outstandingPrefetches(0),
      maxOutstandingPrefetches(params.maxOutstandingPrefetches),
      outstandingTranslations(0),
      maxOutstandingTranslations(params.maxOutstandingTranslations),
      maxPrefetchesPerCycle(params.maxPrefetchesPerCycle),
      cpu(_cpu),
      bac(nullptr), ftq(nullptr),
      decoupledFrontEnd(params.decoupledFrontEnd),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      decodeWidth(params.decodeWidth),
      retryPkt(NULL),
      retryTid(InvalidThreadID),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchBufferMask(fetchBufferSize - 1),
      fetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      icachePort(this, _cpu),
      processTrapEvent(this),
      numPredPerCycle(params.numPredPerCycle),
      fetchStats(_cpu, this)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));
    if (fetchBufferSize > cacheBlkSize)
        fatal("fetch buffer size (%u bytes) is greater than the cache "
              "block size (%u bytes)\n", fetchBufferSize, cacheBlkSize);
    if (cacheBlkSize % fetchBufferSize)
        fatal("cache block (%u bytes) is not a multiple of the "
              "fetch buffer (%u bytes)\n", cacheBlkSize, fetchBufferSize);

    for (int i = 0; i < MaxThreads; i++) {
        fetchStatus[i] = Idle;
        decoder[i] = nullptr;
        pc[i].reset(params.isa[0]->newPCState());
        fetchOffset[i] = 0;
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        memReq[i] = nullptr;
        stalls[i] = {false, false};
        fetchBuffer[i] = NULL;
        fetchBufferPC[i] = 0;
        fetchBufferValid[i] = false;
        lastIcacheStall[i] = 0;
        issuePipelinedIfetch[i] = false;
    }


    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
        // Create space to buffer the cache line data,
        // which may not hold the entire cache line.
        fetchBuffer[tid] = new uint8_t[fetchBufferSize];
    }

    // Get the size of an instruction.
    instSize = decoder[0]->moreBytesSize();
}

std::string Fetch::name() const { return cpu->name() + ".fetch"; }

void
Fetch::regProbePoints()
{
    ppFetch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Fetch");
    ppFetchRequestSent = new ProbePointArg<RequestPtr>(cpu->getProbeManager(),
                                                       "FetchRequest");

}

Fetch::FetchStatGroup::FetchStatGroup(CPU *cpu, Fetch *fetch)
    : statistics::Group(cpu, "fetch"),
    ADD_STAT(predictedBranches, statistics::units::Count::get(),
             "Number of branches that fetch has predicted taken"),
    ADD_STAT(cycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has run and was not squashing or "
             "blocked"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent squashing"),
    ADD_STAT(tlbCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for tlb"),
    ADD_STAT(ftqStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for FTQ to fill."),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle"),
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
    ADD_STAT(miscStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on interrupts, or bad "
             "addresses, or out of MSHRs"),
    ADD_STAT(pendingDrainCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on pipes to drain"),
    ADD_STAT(noActiveThreadStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to no active thread to fetch from"),
    ADD_STAT(pendingTrapStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending traps"),
    ADD_STAT(pendingQuiesceStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending quiesce instructions"),
    ADD_STAT(icacheWaitRetryStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to full MSHR"),
    ADD_STAT(cacheLines, statistics::units::Count::get(),
             "Number of cache lines fetched"),
    ADD_STAT(icacheSquashes, statistics::units::Count::get(),
             "Number of outstanding Icache misses that were squashed"),
    ADD_STAT(tlbSquashes, statistics::units::Count::get(),
             "Number of outstanding ITLB misses that were squashed"),
    ADD_STAT(nisnDist, statistics::units::Count::get(),
             "Number of instructions fetched each cycle (Total)"),
    ADD_STAT(idleRate, statistics::units::Ratio::get(),
             "Ratio of cycles fetch was idle",
             idleCycles / cpu->baseStats.numCycles),
    ADD_STAT(ftNumber, statistics::units::Count::get(),
             "Number of fetch targets processed each cycle (Total)"),

             ADD_STAT(stopFetchReasonReachBranchBW, statistics::units::Count::get(),
             "Number of times fetch was stopped due to branch bandwidth"),
    ADD_STAT(stopFetchReasonReachInstFetchLimit, statistics::units::Count::get(),
             "Number of times fetch was stopped due to instruction fetch limit"),
    ADD_STAT(stopFetchReasonReachFetchBufferLimit, statistics::units::Count::get(),
             "Number of times fetch was stopped due to fetch buffer limit"),
    ADD_STAT(stopFetchReasonReachFTBW, statistics::units::Count::get(),
             "Number of times fetch was stopped due to fetch target bandwidth"),

    ADD_STAT(instrAccessLatency, statistics::units::Count::get(),
             "Demand instruction access latency (in log2(cycles))"),
    ADD_STAT(translationLatency, statistics::units::Count::get(),
             "Translation latency (in log2(cycles))"),
    ADD_STAT(memReqInFlight, statistics::units::Count::get(),
             "Number of memory requests in flight (demand + prefetch)"),
    ADD_STAT(ftReadyToFetch, statistics::units::Count::get(),
             "Number of times a fetch target is ready to fetch"),
    ADD_STAT(ftPrefetchInProgress, statistics::units::Count::get(),
             "Number of times a fetch targets has an outstanding prefetch"),
    ADD_STAT(ftTranslationInProgress, statistics::units::Count::get(),
            "Number of times a fetch targets has an outstanding translation"),
    ADD_STAT(ftTranslationReady, statistics::units::Count::get(),
            "Number of times a fetch targets translation is ready"),
    ADD_STAT(ftTranslationFailed, statistics::units::Count::get(),
            "Number of times a fetch targets translation failed"),
    ADD_STAT(ftCrossCacheBlock, statistics::units::Count::get(),
            "Number of times an instruction crosses a fetch target boundary"),
    ADD_STAT(ftCrossCacheBlockNotNext, statistics::units::Count::get(),
            "Number of times an instruction exceed fetch target boundary"
            " but its not the next fetch target in the FTQ. (x86 branch)"),
    ADD_STAT(demandHit, statistics::units::Count::get(),
            "Number of times demand fetch hits in the icache"),
    ADD_STAT(demandMiss, statistics::units::Count::get(),
            "Number of times demand fetch misses in the icache"),
    ADD_STAT(pfIssued, statistics::units::Count::get(),
            "Number of times a prefetch was sent to the cache"),
    ADD_STAT(pfReceived, statistics::units::Count::get(),
            "Number of times a prefetch was received before fetch needs it"),
    ADD_STAT(pfLate, statistics::units::Count::get(),
            "Number of times a prefetch was late and blocked fetch"),
    ADD_STAT(pfInCache, statistics::units::Count::get(),
            "Number of times a prefetch was already in the cache"),
    ADD_STAT(pfSquashed, statistics::units::Count::get(),
            "Number of times a packet was dropped due to squash. "),
    ADD_STAT(pfLimitReached, statistics::units::Count::get(),
            "Number of times a prefetch was not issues because to many"
            " outstanding."),
    ADD_STAT(pfTranslationLimitReached, statistics::units::Count::get(),
            "Number of times a translation was not issues because to "
            "many outstanding."),
    ADD_STAT(pfAccuracy, statistics::units::Count::get(),
            "The prefetch accuracy"),
    ADD_STAT(pfCoverage, statistics::units::Count::get(),
            "The prefetch coverage")
{
        predictedBranches
            .prereq(predictedBranches);
        cycles
            .prereq(cycles);
        squashCycles
            .prereq(squashCycles);
        tlbCycles
            .prereq(tlbCycles);
        ftqStallCycles
            .prereq(ftqStallCycles);
        idleCycles
            .prereq(idleCycles);
        blockedCycles
            .prereq(blockedCycles);
        cacheLines
            .prereq(cacheLines);
        miscStallCycles
            .prereq(miscStallCycles);
        pendingDrainCycles
            .prereq(pendingDrainCycles);
        noActiveThreadStallCycles
            .prereq(noActiveThreadStallCycles);
        pendingTrapStallCycles
            .prereq(pendingTrapStallCycles);
        pendingQuiesceStallCycles
            .prereq(pendingQuiesceStallCycles);
        icacheWaitRetryStallCycles
            .prereq(icacheWaitRetryStallCycles);
        icacheSquashes
            .prereq(icacheSquashes);
        tlbSquashes
            .prereq(tlbSquashes);
        ftNumber.init(0, fetch->numPredPerCycle, 1);
        nisnDist
            .init(/* base value */ 0,
              /* last value */ fetch->fetchWidth,
              /* bucket size */ 1)
            .flags(statistics::pdf);
        idleRate
            .prereq(idleRate);
        instrAccessLatency
            .init(0,10, 1)
            .flags(statistics::pdf);
        translationLatency
            .init(0,10, 1)
            .flags(statistics::pdf);
        memReqInFlight
            .init(0,10, 1)
            .flags(statistics::pdf);

        pfAccuracy = (pfIssued - pfSquashed) / pfIssued;
        pfCoverage = demandHit / (demandHit + demandMiss);
}
void
Fetch::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);

    // Create a wire to send information to BAC
    toBAC = timeBuffer->getWire(0);
}

void
Fetch::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Fetch::setBACandFTQPtr(BAC *bac_ptr, FTQ * ftq_ptr)
{
    // Set pointer to the fetch target queue
    bac = bac_ptr;
    ftq = ftq_ptr;
}

void
Fetch::setFetchQueue(TimeBuffer<FetchStruct> *ftb_ptr)
{
    // Create wire to write information to proper place in fetch time buf.
    toDecode = ftb_ptr->getWire(0);
}

void
Fetch::startupStage()
{
    assert(bac != nullptr);
    assert(ftq != nullptr);
    assert(priorityList.empty());
    resetStage();

    // Fetch needs to start fetching instructions at the very beginning,
    // so it must start up in active state.
    switchToActive();
}

void
Fetch::clearStates(ThreadID tid)
{
    fetchStatus[tid] = Running;
    set(pc[tid], cpu->pcState(tid));
    fetchOffset[tid] = 0;
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    memReq[tid] = NULL;
    stalls[tid].decode = false;
    stalls[tid].drain = false;
    fetchBufferPC[tid] = 0;
    fetchBufferValid[tid] = false;
    fetchQueue[tid].clear();

    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);

    // Clear out any of this thread's instructions being sent to decode.
    for (int i = -cpu->fetchQueue.getPast();
         i <= cpu->fetchQueue.getFuture(); ++i) {
        FetchStruct& fetch_struct = cpu->fetchQueue[i];
        removeCommThreadInsts(tid, fetch_struct);
    }
}

void
Fetch::resetStage()
{
    numInst = 0;
    interruptPending = false;
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        fetchStatus[tid] = Running;
        set(pc[tid], cpu->pcState(tid));
        fetchOffset[tid] = 0;
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        memReq[tid] = NULL;

        stalls[tid].decode = false;
        stalls[tid].drain = false;

        fetchBufferPC[tid] = 0;
        fetchBufferValid[tid] = false;

        fetchQueue[tid].clear();

        priorityList.push_back(tid);
    }

    wroteToTimeBuffer = false;
    _status = Inactive;
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());
    fetchesInProgress.erase(pkt->req->getPaddr());

    DPRINTF(Fetch, "%s: PA:%#x, VA:%#x\n", __func__,
            pkt->req->getPaddr(), pkt->req->getVaddr());


    if(decoupledFrontEnd) {


    std::vector<FetchTargetPtr> result = ftq->findAll(tid, [pkt](FetchTargetPtr ft) {
        return ft->hasPaddr() && ft->getPaddr() == pkt->req->getPaddr();
    });

    for(auto &ft : result) {
        ft->setFetchBuffer(pkt->getConstPtr<uint8_t>(), fetchBufferSize);
    }

        }

    // First check if the request is the active demand request
    // we are waiting for.
    if (fetchStatus[tid] == IcacheWaitResponse &&
        pkt->req == memReq[tid]) {


        DPRINTF(Fetch, "[tid:%i] Recv.: %#x. Waking up from cache miss.\n", tid,
                pkt->req->getPaddr());
        assert(!cpu->switchedOut());

        memcpy(fetchBuffer[tid], pkt->getConstPtr<uint8_t>(), fetchBufferSize);
        fetchBufferValid[tid] = true;

    DPRINTF(Fetch, "Recv.: %#x, %#x. Copy into FB\n", fetchBuffer[tid][0],
                fetchBuffer[tid][1]);



        // Wake up the CPU (if it went to sleep and was waiting on
        // this completion event).
        cpu->wakeCPU();

        DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
                tid);

        switchToActive();

        // Only switch to IcacheAccessComplete if we're not stalled as well.
        if (checkStall(tid)) {
            fetchStatus[tid] = Blocked;
        } else {
            fetchStatus[tid] = IcacheAccessComplete;
        }

        if (pkt->req->getAccessDepth() == 0) {
            fetchStats.demandHit++;
        } else {
            fetchStats.demandMiss++;
        }

        pkt->req->setAccessLatency();
        auto latency = cpu->ticksToCycles(pkt->req->getAccessLatency());
        fetchStats.instrAccessLatency.sample(
            latency > 0 ? floorLog2(uint64_t(latency)) : 0);
        cpu->ppInstAccessComplete->notify(pkt);

        // Reset the mem req to NULL.
        delete pkt;
        memReq[tid] = NULL;
        return;
    }

    // Not the demand request. Check if belongs to a prefetch.
    if (trySatisfyPrefetch(tid, pkt)) {
        // If the request belongs to a fetch target, we are done
        return;
    }

    // Doesn't belong to anything. Must be a dropped request after a squash.
    ++fetchStats.icacheSquashes;
    delete pkt;
    return;
}

void
Fetch::drainResume()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        stalls[i].decode = false;
        stalls[i].drain = false;
    }
}

void
Fetch::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPkt == NULL);
    assert(retryTid == InvalidThreadID);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(!memReq[i]);
        assert(fetchStatus[i] == Idle || stalls[i].drain);
    }
}

bool
Fetch::isDrained() const
{
    /* Make sure that threads are either idle of that the commit stage
     * has signaled that draining has completed by setting the drain
     * stall flag. This effectively forces the pipeline to be disabled
     * until the whole system is drained (simulation may continue to
     * drain other components).
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify fetch queues are drained
        if (!fetchQueue[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (fetchStatus[i] != Idle) {
            if (fetchStatus[i] == Blocked && stalls[i].drain)
                continue;
            else
                return false;
        }
    }

    /* The pipeline might start up again in the middle of the drain
     * cycle if the finish translation event is scheduled, so make
     * sure that's not the case.
     */
    return !processTrapEvent.scheduled();
}

void
Fetch::takeOverFrom()
{
    assert(cpu->getInstPort().isConnected());
    resetStage();

}

void
Fetch::drainStall(ThreadID tid)
{
    assert(cpu->isDraining());
    assert(!stalls[tid].drain);
    DPRINTF(Drain, "%i: Thread drained.\n", tid);
    stalls[tid].drain = true;
}

void
Fetch::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    fetchStatus[0] = Running;
}

void
Fetch::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::FetchIdx);

        _status = Active;
    }
}

void
Fetch::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);

        _status = Inactive;
    }
}

void
Fetch::deactivateThread(ThreadID tid)
{
    // Update priority list
    auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    if (thread_it != priorityList.end()) {
        priorityList.erase(thread_it);
    }
}

bool
Fetch::ftqReady(ThreadID tid, bool &status_change)
{
    if (!decoupledFrontEnd) return true;
    // If the FTQ is empty wait unit its filled upis available.
    // Need at least two cycles for now.
    if (!ftq->isHeadReady(tid)) {
        fetchStatus[tid] = FTQEmpty;
        status_change = true;
        return false;
    }
    return true;
}


bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc, FetchTargetPtr ft)
{
    Fault fault = NoFault;

    assert(!cpu->switchedOut());

    // @todo: not sure if these should block translation.
    //AlphaDep
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n",
                tid);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n",
                tid);
        return false;
    }

    // Align the fetch address to the start of a fetch buffer segment.
    Addr fetchBufferBlockPC = fetchBufferAlignPC(vaddr);

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for PC:%#x, Addr:%#x\n",
            tid, fetchBufferBlockPC, pc, vaddr);

    if (decoupledFrontEnd) {

        // Read the head fetch target in the FTQ. In theory we only need to
        // read the head. However, for x86 an instruction can span two
        // fetch targets. The PC still points to the fetch target at the head
        // of the FTQ but we need to read a few more bytes from the second
        // fetch target to fully decode the instruction.

        Addr cacheBlock = cacheBlockAligned(vaddr);

        ft = ftq->readHead(tid);
        DPRINTF(Fetch, "Chk %s for %#x\n", ft->print(), cacheBlock);

        assert(ft);
        if (ft->getBlkAddr() != cacheBlock) {
            // If the head of the FTQ is not the right one, check the next
            // fetch target.
            fetchStats.ftCrossCacheBlock++;

            if (ft->isFallThrough()) {
                // If the fetch target falls through sequentially to the next
                // fetch target, we can try using its request.
                ft = ftq->readNextHead(tid);
                DPRINTF(Fetch, "Chk %s for %#x\n", ft->print(), cacheBlock);

                if (ft && ft->getBlkAddr() != cacheBlock) {
                    ft = nullptr;
                    fetchStats.ftCrossCacheBlockNotNext++;
                }
            } else {
                ft = nullptr;
            }
        }


        // if (ft == nullptr) {
        //     int i = 0;
        //     ft = ftq->findNext(tid,
        //         [this, vaddr, &i](FetchTargetPtr &ft) -> bool
        //         {
        //             i++;
        //             DPRINTF(Fetch, "Chk %s for %#x\n", ft->print(), vaddr);
        //             return ft->inRangeAligned(vaddr, fetchBufferSize);
        //         });
        //     assert((i<=2) && ft);
        // }

    }

    if (ft) {

        bool done = false;
        switch(ft->state) {

            case FetchTarget::ReadyToFetch:
                // If the fetch target is ready to fetch, we can initiate the
                // cache access right away. Translation is already done. And
                // the block was prefetched into the I-cache.
                DPRINTF(Fetch, "[tid:%i] Ready to fetch: %s\n",
                        tid, ft->print());
                fetchStats.ftReadyToFetch++;
                break;


            case FetchTarget::PrefetchInProgress:
                // If the prefetch is still in progress, we wait for it's
                // response. The prefetch will become the actual demand
                // request.
                DPRINTF(Fetch, "[tid:%i] Prefetch in progress: %s\n",
                        tid, ft->print());
                fetchStats.ftPrefetchInProgress++;
                fetchStats.pfLate++;

                // Prefetch will become the demand request.
                outstandingPrefetches--;
                lastIcacheStall[tid] = curTick();
                fetchStatus[tid] = IcacheWaitResponse;
                fetchBufferPC[tid] = fetchBufferBlockPC;
                fetchBufferValid[tid] = false;
                memReq[tid] = ft->popReq();
                ft->markReady();

                // Notify Fetch Request probe when the packet becomes a
                // demand request.
                ppFetchRequestSent->notify(memReq[tid]);
                done = true;
                break;



        // At this point we know the prefetch was not issued yet.
        // Next remaining states check the translation state.


            case FetchTarget::TranslationInProgress:
                // If the fetch target translation is in progress,
                // we need to wait for it to complete.
                DPRINTF(Fetch, "[tid:%i] Translation in progress: %s\n",
                        tid, ft->print());
                fetchStats.ftTranslationInProgress++;

                fetchStatus[tid] = ItlbWait;
                memReq[tid] = ft->popReq();
                ft->markReady();
                done = true;
                break;


            case FetchTarget::TranslationFailed:
                // If the fetch target translation failed, we need
                // to pop the fault and execute the trap handler.
                DPRINTF(Fetch, "[tid:%i] Translation failed: %s\n",
                        tid, ft->print());
                fetchStats.ftTranslationFailed++;
                processTrap(tid, ft->fault, ft->req);
                done = true;
                break;


            case FetchTarget::TranslationReady:
                // If the fetch target translation is ready, we can
                // initiate the cache access right away. Since the request
                // was not used for prefetching we can use it directly.
                DPRINTF(Fetch, "[tid:%i] Translation ready: %s\n",
                        tid, ft->print());
                fetchStats.ftTranslationReady++;
                break;

            default:
                assert(ft->initial());
        }

        if (done) {
            return true;
        }
    }

    // Create a new request for the fetch buffer block.
    memReq[tid] = makeRequest(fetchBufferBlockPC, tid, pc, ft);

    // If the request has already a valid physical address, we can
    // skip translation and initiate the cache access right away.
    if (memReq[tid]->hasPaddr()) {
        performCacheAccess(fetchBufferBlockPC, tid, memReq[tid]);
    } else {
        // Initiate translation of the icache block
        fetchStatus[tid] = ItlbWait;
        startTranslation(memReq[tid], tid, ft);
    }

    return true;
}

void
Fetch::startTranslation(const RequestPtr &mem_req, const ThreadID tid,
                        const FetchTargetPtr &ft)
{
    if (ft) ft->startTranslation(mem_req);

    outstandingTranslations++; // Increment must happen before as the
                              // translation may complete immediately.
    FetchTranslation *trans = new FetchTranslation(this, ft);
    cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);
}

RequestPtr
Fetch::makeRequest(Addr vaddr, ThreadID tid, Addr pc, FetchTargetPtr ft)
{
    RequestPtr req = nullptr;

    // First check if we can reuse the request from the fetch target.
    if (ft && ft->req && ft->req->getVaddr() == vaddr) {
        req = ft->popReq();
        ft->markReady();
        DPRINTF(Fetch, "[tid:%i] Reusing request for %#x from %s\n",
                tid, vaddr, ft->print());
    }

    // Setup the memReq to do a read of the first instruction's address.
    // Set the appropriate read size and flags as well.
    // Build request here.
    if (!req) {
        req = std::make_shared<Request>(
            vaddr, fetchBufferSize,
            Request::INST_FETCH, cpu->instRequestorId(), pc,
            cpu->thread[tid]->contextId());

        req->taskId(cpu->taskId());
    }

    if (ft && ft->hasPaddr()
           && ft->getBlkAddr() == cacheBlockAligned(vaddr)) {

        // Get the physical address from the fetch target.
        // Note that the fetch target covers a whole cache block.
        // Take only cache block address and add the fetch
        // buffer offset.
        // Problem: in x86 an instruction can cross a cache line
        // boundary. The PC start might still be this fetch target
        // but we need to fetch the next cache line in order to
        // decode the full instruction. We handle this by
        // checking the fetch target range and do the translation
        // again.

        DPRINTF(Fetch, "[tid:%i] Using translation VA:%#x, PA:%#x from %s\n",
            tid, vaddr, ft->getPaddr(), ft->print());
        Addr cl_pa = ft->getPaddr() & ~(uint64_t(cacheBlkSize - 1));
        cl_pa += vaddr & (cacheBlkSize - 1);

        req->setPaddr(cl_pa);
        DPRINTF(Fetch, "[tid:%i] Using translation VA:%#x, PA:%#x from %s\n",
                tid, vaddr, cl_pa, ft->print());
    }
    return req;
}


bool
Fetch::performCacheAccess(Addr vaddr, ThreadID tid, const RequestPtr &mem_req,
                          bool prefetch)
{
    // Check that we're not going off into random memory
    // If we have, just wait around for commit to squash something and put
    // us on the right track
    if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
        warn("%llu Address %#x is outside of physical memory, stopping fetch\n",
             curTick(), mem_req->getPaddr());
        DPRINTF(Fetch, "[tid:%i] Address %#x is outside of physical memory\n",
                tid, mem_req->getPaddr());
        fetchStatus[tid] = NoGoodAddr;
        memReq[tid] = NULL;
        return false;
    }

    // Build packet here.
    PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
    data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);

    if (!prefetch) {
        fetchBufferPC[tid] = vaddr;
        fetchBufferValid[tid] = false;
        DPRINTF(Fetch, "Fetch: Doing instruction read. VA:%#lx, PA:%#lx\n",
                vaddr, mem_req->getPaddr());
        assert(vaddr == mem_req->getVaddr());

        fetchStats.cacheLines++;
    }


    // Access the cache.
    if (!icachePort.sendTimingReq(data_pkt)) {
        assert(retryPkt == NULL);
        assert(retryTid == InvalidThreadID);
        DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

        if (prefetch) {
            // If we're doing a prefetch, we can just drop the packet
            // and not worry about it.
            delete data_pkt;
        } else {
            // Otherwise, we need to save the packet and try again later.
            fetchStatus[tid] = IcacheWaitRetry;
            retryPkt = data_pkt;
            retryTid = tid;
            cacheBlocked = true;
        }
        return false;
    }

    // Keep track of the outstanding fetches.
    fetchesInProgress.insert(mem_req->getPaddr());
    DPRINTF(Fetch, "[tid:%i] Successful send %s request to %#x. "
            "In-flight: %i.\n", tid, prefetch ? "prefetch" : "demand fetch",
            mem_req->getPaddr(),
            fetchesInProgress.size());
    fetchStats.memReqInFlight.sample(fetchesInProgress.size());

    // Successful send
    if (!prefetch) {
        DPRINTF(Fetch, "[tid:%i] Doing demand Icache access.\n", tid);
        DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache "
                "response.\n", tid);

        // Demand access blocks the CPU until the response returns.
        lastIcacheStall[tid] = curTick();
        fetchStatus[tid] = IcacheWaitResponse;

        // Notify Fetch Request probe when a packet containing a fetch
        // request is successfully sent
        ppFetchRequestSent->notify(mem_req);
    }
    return true;
}


void
Fetch::processFTQ(const ThreadID tid)
{
    // To prefetch there must be at least one other fetch target apart
    // from the head FT in the FTQ.
    if (ftq->size(tid) < 2) return;
    if (!ftq->isValid(tid)) return;

    FetchTargetPtr ft = nullptr;
    ftq->printFTQ(tid);

    for (int i = 0; i < maxPrefetchesPerCycle; i++) {

        // Prefetch Translations ----------------------------------------
        if (outstandingTranslations < maxOutstandingTranslations) {

            // First check if the FTQ contains fetch targets that
            // require a translation.
            ft = ftq->findAfterHead(tid,
                [this](FetchTargetPtr &ft) -> bool
                {
                    return ft->requiresTranslation();
                });

            if (ft != nullptr) {
                // Send translation request to the MMU.
                Addr fetchBufferBlockPC = fetchBufferAlignPC(ft->startAddress());
                auto req = makeRequest(fetchBufferBlockPC,
                                        tid, ft->startAddress());

                DPRINTF(Fetch, "[tid:%i] Translation for %#x started %s\n",
                        tid, fetchBufferBlockPC, ft->print());

                startTranslation(req, tid, ft);
            }

        } else {

            // If we have too many outstanding prefetches, we can't issue
            // more.
            DPRINTF(Fetch, "[tid:%i] Can't issue translation, too many "
                    "outstanding\n", tid);
            fetchStats.pfTranslationLimitReached++;
        }
    }

    // Prefetch -------------------------------------------
    if ((retryPkt != NULL) || cacheBlocked) {
        // If there are packets in the retry queue, we can't issue
        // prefetches.
        DPRINTF(Fetch, "[tid:%i] Can't issue prefetches, out of MSHRs\n",
                tid);
        return;
    }
    ftq->printFTQ(tid);

    for (int i = 0; i < maxPrefetchesPerCycle; i++) {

        if (outstandingPrefetches >= maxOutstandingPrefetches) {
            // If we have too many outstanding prefetches, we can't issue
            // more.
            DPRINTF(Fetch, "[tid:%i] Can't issue prefetches, too many "
                    "outstanding\n", tid);
            fetchStats.pfLimitReached++;
            return;
        }

        // The front-end is able to prefetch. Search for the next fetch target
        // that can be prefetched.
        ft = ftq->findAfterHead(tid,
            [this](FetchTargetPtr &ft) -> bool
            {
                return ft->translationReady();
            });

        if (ft != nullptr) {
            // Send prefetch request to the cache.
            RequestPtr req = ft->req;
            assert(req != nullptr);

            // Check if an access to this address is already in flight.
            auto it = fetchesInProgress.find(req->getPaddr());
            if (it != fetchesInProgress.end()) {
                DPRINTF(Fetch, "[tid:%i] Access to %#x/%#x already in flight. "
                        "Mark ready\n", tid, req->getVaddr(), req->getPaddr());
                ft->markReady();
            } else if (performCacheAccess(req->getVaddr(), tid, req, true)) {
                ft->prefetchIssued();
                outstandingPrefetches++;
                fetchStats.pfIssued++;

                DPRINTF(Fetch, "[tid:%i] Prefetch request send %#x (%i/%i) %s\n",
                    tid, req->getVaddr(),
                    outstandingPrefetches, maxOutstandingPrefetches,
                    ft->print());
            }
        }
    }
}



bool
Fetch::isPrefetchTranslation(const ThreadID tid, const Fault &fault,
                             const RequestPtr &mem_req)
{
    if (!decoupledFrontEnd) return false;

    // Iterate over all fetch targets in the FTQ and check if the
    // request belongs to one of them.
    FetchTargetPtr ft = ftq->findAfterHead(tid,
        [this, mem_req](FetchTargetPtr &ft) -> bool
        {
            return ft->req == mem_req;
        });

    // If no fetch target was found, return false.
    if (ft == nullptr) return false;


    DPRINTF(Fetch, "[tid:%i] Translation for PF:%#x completed %s with %i\n",
            tid, mem_req->getVaddr(), ft->print(), fault);

    ft->finishTranslation(fault, mem_req, true);
    return true;
}

bool
Fetch::trySatisfyPrefetch(const ThreadID tid, PacketPtr pkt)
{
    if (!decoupledFrontEnd) return false;

    // Iterate over all fetch targets in the FTQ and check if the
    // request belongs to one of them.
    FetchTargetPtr ft = ftq->findNext(tid,
        [this, pkt](FetchTargetPtr &ft) -> bool
        {
            return ft->req == pkt->req;
        });

    // If no fetch target was found, return.
    if (ft == nullptr) return false;

    DPRINTF(Fetch, "[tid:%i] Prefetch for %#x completed %s\n",
            tid, pkt->req->getVaddr(), ft->print());

    // All (translation and prefetch) done for this fetch target.
    // Delete the request and the packet.
    ft->markReady();
    outstandingPrefetches--;
    fetchStats.pfReceived++;
    if (pkt->req->getAccessDepth() == 0) {
        fetchStats.pfInCache++;
    }
    delete pkt;
    return true;
}


void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req,
                         const FetchTargetPtr &ft)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());
    Addr fetchBufferBlockPC = mem_req->getVaddr();

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    outstandingTranslations--;
    DPRINTF(Fetch, "[tid:%i] Translation for %#x completed with %s\n",
        tid, mem_req->getVaddr(),
        fault==NoFault ? "NoFault" : "Fault");

    // if (fetchStatus[tid] != ItlbWait || mem_req != memReq[tid] ||
    //     mem_req->getVaddr() != memReq[tid]->getVaddr()) {


    //     if (ft && ft->isValid()) {
    //         DPRINTF(Fetch, "[tid:%i] Translation for %#x completed %s\n",
    //                 tid, mem_req->getVaddr(), ft->print());

    //         auto lat = ft->finishTranslation(fault, mem_req, true);
    //         fetchStats.translationLatency.sample(lat ? floorLog2(lat) : 0);
    //     } else {

    //         // The request is neither for the head nor for a fetch target.
    //         DPRINTF(Fetch, "[tid:%i] Ignoring itlb completed after squash\n",
    //                 tid);
    //         ++fetchStats.tlbSquashes;
    //     }

    //     // In either we case we are done here.
    //     return;
    // }
    bool demand_request = (fetchStatus[tid] == ItlbWait) &&
                          mem_req == memReq[tid] &&
                          mem_req->getVaddr() == memReq[tid]->getVaddr();

    if (ft && ft->isValid()) {
        DPRINTF(Fetch, "[tid:%i] Translation for %#x completed %s with %i\n",
                tid, mem_req->getVaddr(), ft->print(),
                fault==NoFault ? "NoFault" : "Fault");
        DPRINTF(Fetch, "Fetch: Doing instruction read. VA:%#lx, PA:%#lx\n",
                mem_req->getVaddr(), fault==NoFault ? mem_req->getPaddr() : 0);

        auto lat = ft->finishTranslation(fault, mem_req, !demand_request);
        fetchStats.translationLatency.sample(lat ? floorLog2(lat) : 0);
    }

    if (!demand_request) {

        if (!ft || !ft->isValid()) {
            DPRINTF(Fetch, "[tid:%i] Ignoring itlb completed after squash\n",
                    tid);
            ++fetchStats.tlbSquashes;
        }
        // The request is neither for the head nor for a fetch target.
        return;
    }


    // If translation was successful, attempt to read the icache block.
    if (fault == NoFault) {
        performCacheAccess(fetchBufferBlockPC, tid, mem_req);
    } else {
        processTrap(tid, fault, mem_req);
    }
    _status = updateFetchStatus();
}


void
Fetch::processTrap(const ThreadID tid, const Fault &fault,
                   const RequestPtr &mem_req)
{

    // Don't send an instruction to decode if we can't handle it.
    if (!(numInst < fetchWidth) ||
            !(fetchQueue[tid].size() < fetchQueueSize)) {
        if (!processTrapEvent.scheduled()) {
            processTrapEvent.setup(tid, fault, mem_req);
            cpu->schedule(processTrapEvent,
                cpu->clockEdge(Cycles(1)));
        }
        return;
    }


    DPRINTF(Fetch,
            "[tid:%i] Got back req:%#x with VA:%#x \n",
            tid, mem_req, mem_req->getVaddr());
    // Translation faulted, icache request won't be sent.
    memReq[tid] = NULL;

    // Send the fault to commit.  This thread will not do anything
    // until commit handles the fault.  The only other way it can
    // wake up is if a squash comes along and changes the PC.
    const PCStateBase &fetch_pc = *pc[tid];

    DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
    // We will use a nop in order to carry the fault.
    DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
            fetch_pc, fetch_pc, false);
    instruction->setNotAnInst();

    instruction->setPredTarg(fetch_pc);
    instruction->fault = fault;
    wroteToTimeBuffer = true;

    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();

    fetchStatus[tid] = TrapPending;

    DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
    DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
            tid, fault->name(), *pc[tid]);
}


void
Fetch::squashFromDecode(const PCStateBase &new_pc,
                        const DynInstPtr squashInst,
                        const InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(new_pc, squashInst, tid);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}


void
Fetch::squash(const PCStateBase &new_pc, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(new_pc, squashInst, tid);

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}


void
Fetch::doSquash(const PCStateBase &new_pc, const DynInstPtr squashInst,
        ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s.\n",
            tid, new_pc);

    set(pc[tid], new_pc);
    fetchOffset[tid] = 0;
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr() &&
        !squashInst->isLastMicroop())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    if (fetchStatus[tid] == IcacheWaitResponse) {
        DPRINTF(Fetch, "[tid:%i] Squashing outstanding Icache miss.\n",
                tid);
        memReq[tid] = NULL;
    } else if (fetchStatus[tid] == ItlbWait) {
        DPRINTF(Fetch, "[tid:%i] Squashing outstanding ITLB miss.\n",
                tid);
        memReq[tid] = NULL;
    }

    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == tid) {
        assert(cacheBlocked);
        if (retryPkt) {
            delete retryPkt;
        }
        retryPkt = NULL;
        retryTid = InvalidThreadID;
    }

    fetchStatus[tid] = Squashing;

    // Empty fetch queue
    fetchQueue[tid].clear();

    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    // Drop all prefetches
    fetchStats.pfSquashed += outstandingPrefetches;
    outstandingPrefetches = 0;

    if (processTrapEvent.scheduled()) {
        processTrapEvent.squash();
    }

    ++fetchStats.squashCycles;
}


void
Fetch::bacResteer(const PCStateBase &new_pc, ThreadID tid)
{
    DPRINTF(Fetch,"[tid:%i] Resteer BAC to PC: %s\n",tid, new_pc);

    toBAC->fetchInfo[tid].squash = true;
    set(toBAC->fetchInfo[tid].nextPC, new_pc);
    // Also invalidate FTQ. Shall be fixed from BAC.
    ftq->invalidate(tid);
}

bool
Fetch::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].drain) {
        assert(cpu->isDraining());
        DPRINTF(Fetch,"[tid:%i] Drain stall detected.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    //Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == Squashing ||
            fetchStatus[tid] == IcacheAccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (fetchStatus[tid] == IcacheAccessComplete) {
                    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                            "completion\n",tid);
                }

                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

void
Fetch::tick()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;

    for (ThreadID i = 0; i < numThreads; ++i) {
        issuePipelinedIfetch[i] = false;
    }

    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change = status_change || updated_status;
    }

    DPRINTF(Fetch, "Running stage.\n");

    if (FullSystem) {
        if (fromCommit->commitInfo[0].interruptPending) {
            interruptPending = true;
        }

        if (fromCommit->commitInfo[0].clearInterrupt) {
            interruptPending = false;
        }
    }

    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        // Fetch each of the actively fetching threads.
        fetch(status_change);
    }

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }

    // Issue the next I-cache request if possible.
    for (ThreadID i = 0; i < numThreads; ++i) {
        if (issuePipelinedIfetch[i]) {
            pipelineIcacheAccesses(i);
        }
    }

    // Process prefetches
    if (decoupledFrontEnd) {
        for (ThreadID i = 0; i < numThreads; ++i) {
            processFTQ(i);
        }
    }

    // Send instructions enqueued into the fetch queue to decode.
    // Limit rate by fetchWidth.  Stall if decode is stalled.
    unsigned insts_to_decode = 0;
    unsigned available_insts = 0;

    for (auto tid : *activeThreads) {
        if (!stalls[tid].decode) {
            available_insts += fetchQueue[tid].size();
        }
    }

    // Pick a random thread to start trying to grab instructions from
    auto tid_itr = activeThreads->begin();
    std::advance(tid_itr,
            rng->random<uint8_t>(0, activeThreads->size() - 1));

    while (available_insts != 0 && insts_to_decode < decodeWidth) {
        ThreadID tid = *tid_itr;
        if (!stalls[tid].decode && !fetchQueue[tid].empty()) {
            const auto& inst = fetchQueue[tid].front();
            toDecode->insts[toDecode->size++] = inst;
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                    "from fetch queue. Fetch queue size: %i.\n",
                    tid, inst->seqNum, fetchQueue[tid].size());

            wroteToTimeBuffer = true;
            fetchQueue[tid].pop_front();
            insts_to_decode++;
            available_insts--;
        }

        tid_itr++;
        // Wrap around if at end of active threads list
        if (tid_itr == activeThreads->end())
            tid_itr = activeThreads->begin();
    }

    // If there was activity this cycle, inform the CPU of it.
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    // Reset the number of the instruction we've fetched.
    numInst = 0;
}

bool
Fetch::checkSignalsAndUpdate(ThreadID tid)
{
    // Update the per thread stall statuses.
    if (fromDecode->decodeBlock[tid]) {
        stalls[tid].decode = true;
    }

    if (fromDecode->decodeUnblock[tid]) {
        assert(stalls[tid].decode);
        assert(!fromDecode->decodeBlock[tid]);
        stalls[tid].decode = false;
    }

    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing from commit with PC = %s\n",
                tid, *fromCommit->commitInfo[tid].pc);

        squash(*fromCommit->commitInfo[tid].pc,
               fromCommit->commitInfo[tid].doneSeqNum,
               fromCommit->commitInfo[tid].squashInst, tid);
        return true;
    }

    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash
        && (fetchStatus[tid] != Squashing)) {
        // Squash unless we're already squashing

        DPRINTF(Fetch, "[tid:%i] Squashing from decode with PC = %s\n",
                tid, *fromDecode->decodeInfo[tid].nextPC);

        squashFromDecode(*fromDecode->decodeInfo[tid].nextPC,
                            fromDecode->decodeInfo[tid].squashInst,
                            fromDecode->decodeInfo[tid].doneSeqNum,
                            tid);

        return true;
    }

    if (checkStall(tid) &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != FTQEmpty &&
        fetchStatus[tid] != QuiescePending) {
        DPRINTF(Fetch, "[tid:%i] Setting to blocked\n",tid);

        fetchStatus[tid] = Blocked;

        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        // With a decoupled front-end we can only to running if the FTQ
        // is not empty otherwise we need to wait to fillup.
        if (decoupledFrontEnd && ftq->isEmpty(tid)) {
            fetchStatus[tid] = FTQEmpty;
        } else {
            DPRINTF(Fetch,
                    "[tid:%i] Done squashing, switching to running.\n", tid);

            fetchStatus[tid] = Running;
        }
        return true;
    }

    // Check if the FTQ in not empty anymore.
    if (fetchStatus[tid] == FTQEmpty && !ftq->isEmpty(tid)) {
        DPRINTF(Fetch, "[tid:%i] FTQ is refilled -> running\n", tid);
        fetchStatus[tid] = Running;
        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status. Fetch remains the same as before.
    return false;
}

DynInstPtr
Fetch::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    DynInst::Arrays arrays;
    arrays.numSrcs = staticInst->numSrcRegs();
    arrays.numDests = staticInst->numDestRegs();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, staticInst, curMacroop, this_pc, next_pc, seq, cpu);
    instruction->setTid(tid);

    instruction->setThreadState(cpu->thread[tid]);

    DPRINTF(Fetch, "[tid:%i] Instruction PC %s created [sn:%lli].\n",
            tid, this_pc, seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->disassemble(this_pc.instAddr()));

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, this_pc, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Write the instruction to the first slot in the queue
    // that heads to decode.
    assert(numInst < fetchWidth);
    fetchQueue[tid].push_back(instruction);
    assert(fetchQueue[tid].size() <= fetchQueueSize);
    DPRINTF(Fetch, "[tid:%i] Fetch queue entry created (%i/%i).\n",
            tid, fetchQueue[tid].size(), fetchQueueSize);
    //toDecode->insts[toDecode->size++] = instruction;

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    return instruction;
}



bool
Fetch::fetchTargetHasFBReady(ThreadID tid, bool &status_change, FetchTargetPtr curFT, Addr fetchAddr) {


return ftqReady(tid, status_change) && curFT && curFT->hasFetchBuffer() && fetchBufferAlignPC(curFT->startAddress()) == fetchBufferAlignPC(fetchAddr);

}


void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    ThreadID tid = getFetchingThread();

    assert(!cpu->switchedOut());

    if (tid == InvalidThreadID) {
        // Breaks looping condition in tick()
        threadFetched = numFetchingThreads;

        if (numThreads == 1) {  // @todo Per-thread stats
            profileStall(0);
        }

        return;
    }

    // Check if the FTQ is ready and process the tail fetch target.
    // In the non decoupled front-end ftqReady() will always return true;
    if (!ftqReady(tid, status_change)) {
        DPRINTF(Fetch, "FTQ not ready [tid:%i]\n", tid);

        // No fetch target. We don't know what to fetch.
        ++fetchStats.ftqStallCycles;
        return;
    }

    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC.
    PCStateBase &this_pc = *pc[tid];
    Addr pcOffset = fetchOffset[tid];
    Addr fetchAddr = (this_pc.instAddr() + pcOffset) & decoder[tid]->pcMask();

    bool inRom = isRomMicroPC(this_pc.microPC());

    FetchTargetPtr curFT = ftq->readHead(tid);

    if (decoupledFrontEnd) { // #ifdef FDIP
        assert(ftqReady(tid,status_change));

        if (!curFT->inRange(this_pc.instAddr())) {
            DPRINTF(Fetch, "[tid:%i] PC:%#x not within fetch target: %s\n",
                            tid, this_pc, curFT->print());
            bacResteer(this_pc, tid);
            ++fetchStats.ftqStallCycles;
            return;
        }
    }

    // If returning from the delay of a cache miss, then update the status
    // to running, otherwise do the cache access.  Possibly move this up
    // to tick() function.
    if (fetchStatus[tid] == IcacheAccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);

        fetchStatus[tid] = Running;
        status_change = true;
    } else if (fetchStatus[tid] == Running) {
        // Align the fetch PC so its at the start of a fetch buffer segment.
        Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

        // If buffer is no longer valid or fetchAddr has moved to point
        // to the next cache block, AND we have no remaining ucode
        // from a macro-op, then start fetch from icache.
        if (!((fetchBufferValid[tid] && ftqReady(tid, status_change) && fetchBufferBlockPC == fetchBufferPC[tid])
         || fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr))
         && !inRom && !macroop[tid]) {
            DPRINTF(Fetch, "[tid:%i] Attempting to translate and read "
                    "instruction, starting at PC %s.\n", tid, this_pc);

            fetchCacheLine(fetchAddr, tid, this_pc.instAddr());

            if (fetchStatus[tid] == IcacheWaitResponse) {
                cpu->fetchStats[tid]->icacheStallCycles++;
            }
            else if (fetchStatus[tid] == ItlbWait)
                ++fetchStats.tlbCycles;
            else if (fetchStatus[tid] == FTQEmpty)
                ++fetchStats.ftqStallCycles;
            else
                ++fetchStats.miscStallCycles;
            return;
        } else if (checkInterrupt(this_pc.instAddr()) &&
                !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted and we're not issuing
            // an delayed commit micro-op currently (delayed commit
            // instructions are not interruptable by interrupts, only faults)
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return;
        }
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }

        // Status is Idle, so fetch should do nothing.
        return;
    }

    ++fetchStats.cycles;
    std::unique_ptr<PCStateBase> next_pc(this_pc.clone());

    StaticInstPtr staticInst = NULL;
    StaticInstPtr curMacroop = macroop[tid];

    // If the read of the first instruction was successful, then grab the
    // instructions from the rest of the cache line and put them into the
    // queue heading to decode.

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to "
            "decode.\n", tid);

    // Need to keep track of whether or not a predicted branch
    // ended this fetch block.
    bool predictedBranch = false;
    bool mispredict = false;
    unsigned ftCount = 0;

    // Need to halt fetch if quiesce instruction detected
    bool quiesce = false;

    const unsigned numInsts = fetchBufferSize / instSize;
    unsigned blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;

    if(fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr)) {

        blkOffset = (fetchAddr - fetchBufferAlignPC(curFT->startAddress())) / instSize;

    }


    auto *dec_ptr = decoder[tid];
    const Addr pc_mask = dec_ptr->pcMask();

    // Loop through instruction memory from the cache.
    // Keep issuing while fetchWidth is available and branch is not
    // predicted taken
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize
            && !quiesce && !mispredict) {

        // For the decoupled front-end also check if the FTQ
        // and the fetch target are still valid.
        if (decoupledFrontEnd && (!ftq->isValid(tid) || !curFT)) {
            break;
        }
        assert(!curFT || curFT->inRange(this_pc.instAddr()));

        // We need to process more memory if we aren't going to get a
        // StaticInst from the rom, the current macroop, or what's already
        // in the decoder.
        bool needMem = !inRom && !curMacroop && !dec_ptr->instReady();
        fetchAddr = (this_pc.instAddr() + pcOffset) & pc_mask;
        Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
        if (needMem) {
            // If buffer is no longer valid or fetchAddr has moved to point
            // to the next cache block then start fetch from icache.
            if (!fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr) && (!fetchBufferValid[tid] ||
                fetchBufferBlockPC != fetchBufferPC[tid]))
                break;

            if (blkOffset >= numInsts) {
                // We need to process more memory, but we've run out of the
                // current block.
                break;
            }

            const u_int8_t *l_buffer = fetchBuffer[tid];
            if(fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr)) {
                l_buffer = curFT->getFetchBuffer();
            }

            memcpy(dec_ptr->moreBytesPtr(),
            l_buffer + blkOffset * instSize, instSize);
            DPRINTF(Fetch, "Copy bytes %#x from %#x to %#x\n",
                    uint64_t(l_buffer + blkOffset * instSize),
                    fetchAddr, fetchAddr + instSize);
            decoder[tid]->moreBytes(this_pc, fetchAddr);

            if (dec_ptr->needMoreBytes()) {
                blkOffset++;
                fetchAddr += instSize;
                pcOffset += instSize;
            }
        }

        // Extract as many instructions and/or microops as we can from
        // the memory we've processed so far.
        do {
            if (!(curMacroop || inRom)) {
                if (dec_ptr->instReady()) {
                    staticInst = dec_ptr->decode(this_pc);

                    // Increment stat of fetched instructions.
                    cpu->fetchStats[tid]->numInsts++;

                    if (staticInst->isMacroop()) {
                        curMacroop = staticInst;
                    } else {
                        pcOffset = 0;
                    }
                } else {
                    // We need more bytes for this instruction so blkOffset and
                    // pcOffset will be updated
                    break;
                }
            }
            // Whether we're moving to a new macroop because we're at the
            // end of the current one, or the branch predictor incorrectly
            // thinks we are...
            bool newMacro = false;
            if (curMacroop || inRom) {
                if (inRom) {
                    staticInst = dec_ptr->fetchRomMicroop(
                            this_pc.microPC(), curMacroop);
                } else {
                    staticInst = curMacroop->fetchMicroop(this_pc.microPC());
                }
                newMacro |= staticInst->isLastMicroop();
            }

            DynInstPtr instruction = buildInst(
                    tid, staticInst, curMacroop, this_pc, *next_pc, true);

            ppFetch->notify(instruction);
            numInst++;

#if TRACING_ON
            if (debug::O3PipeView) {
                instruction->fetchTick = curTick();
            }
#endif

            set(next_pc, this_pc);

            // If we're branching after this instruction, quit fetching
            // from the same block.
            predictedBranch |= this_pc.branching();

            // Get the next PC from the BAC stage.
            predictedBranch |= bac->updatePC(instruction, *next_pc, curFT);

            if (instruction->isControl()) {
                cpu->fetchStats[tid]->numBranches++;
            }
            if (predictedBranch) {
                DPRINTF(Fetch, "Branch detected with PC = %s -> targ: %s, \n",
                                this_pc, *next_pc);
                ++fetchStats.predictedBranches;
            }

            newMacro |= this_pc.instAddr() != next_pc->instAddr();

            // Move to the next instruction, unless we have a branch.
            set(this_pc, *next_pc);
            inRom = isRomMicroPC(this_pc.microPC());

            if (newMacro) {

                fetchAddr = this_pc.instAddr() & pc_mask;
                blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;
                if(fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr)) {
                    blkOffset = (fetchAddr - fetchBufferAlignPC(curFT->startAddress())) / instSize;
                }
                pcOffset = 0;
                curMacroop = NULL;
            }

            // Check if the PC exceed the fetch target.
            // The pointer is null in the non-decoupled case.
            if (curFT && !curFT->inRange(this_pc.instAddr())) {
                curFT = nullptr;
            }

            if (instruction->isQuiesce()) {
                DPRINTF(Fetch,
                        "Quiesce instruction encountered, halting fetch!\n");
                fetchStatus[tid] = QuiescePending;
                status_change = true;
                quiesce = true;
                break;
            }
            //If the current FT is consumed, pop the head and read the next one
            if (decoupledFrontEnd && !curFT) {
                ftCount++;
                if(ftq->updateHead(tid)){
                    if(ftCount <  numPredPerCycle)
                    {
                    curFT = ftq->readHead(tid);
                    if(fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr)){
                        blkOffset = (fetchAddr - fetchBufferAlignPC(curFT->startAddress())) / instSize;
                    }


                }
                } else {
            // The update was not successful. The BPU predicted something
            // wrong. Squash the FTQ.
                    mispredict = true;
                }
                break;

            }
        } while ((curMacroop || dec_ptr->instReady()) &&
                 numInst < fetchWidth &&
                 fetchQueue[tid].size() < fetchQueueSize);

        // Re-evaluate whether the next instruction to fetch is in micro-op ROM
        // or not.
        inRom = isRomMicroPC(this_pc.microPC());
    }
    DPRINTF(Fetch, "Done fetching: ftHasFB: %s, "
            "predictedBranch: %s, numInst: %i, blkOffset: %i, "
            "fetchQueue[tid].size(): %i, numInsts: %i, "
            "curMacroop: %i, dec_ptr->instReady(): %i\n",
            fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr),
            predictedBranch ? "true" : "false",
            numInst, blkOffset, fetchQueue[tid].size(), numInsts,
            curMacroop ? 1 : 0, dec_ptr->instReady() ? 1 : 0);

    if (!fetchTargetHasFBReady(tid, status_change, curFT, fetchAddr)) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached end of "
                "fetch buffer.\n", tid);
        fetchStats.stopFetchReasonReachFetchBufferLimit++;
    }
    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, predicted branch "
                "instruction encountered.\n", tid);
        fetchStats.stopFetchReasonReachBranchBW++;
    } 
    if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth "
                "for this cycle.\n", tid);
        fetchStats.stopFetchReasonReachInstFetchLimit++;
    }
    if ((blkOffset >= fetchBufferSize) || (blkOffset >= numInsts)) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached the end "
                "fetch buffer.\n", tid);
        fetchStats.stopFetchReasonReachFetchBufferLimit++;
    }
    if (decoupledFrontEnd && !curFT) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached end of fetch "
                "target.\n", tid);
        fetchStats.stopFetchReasonReachFTBW++;
    }
    if (fetchQueue[tid].size() >= fetchQueueSize) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch queue "
                "limit.\n", tid);
        fetchStats.stopFetchReasonReachFetchQueueLimit++;
    }
    if (quiesce) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, quiesce instruction "
                "encountered.\n", tid);
        fetchStats.stopFetchReasonQuiesce++;
    }
    if (mispredict) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, mispredict detected.\n", tid);
        fetchStats.stopFetchReasonMispredict++;
    }
    if (!(curMacroop || dec_ptr->instReady())) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, decoder is not ready.\n", tid);
        fetchStats.stopFetchReasonDecoderNotReady++;
    }

    fetchStats.ftNumber.sample(ftCount);

    //is mispredict detected, we are squashing the ftq
    if (decoupledFrontEnd && mispredict) {
        DPRINTF(Fetch, "Mispredict detected, squashing the FTQ.\n");
            bacResteer(this_pc, tid);
    }

    macroop[tid] = curMacroop;
    fetchOffset[tid] = pcOffset;

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    // pipeline a fetch if we're crossing a fetch buffer boundary and not in
    // a state that would preclude fetching
    fetchAddr = (this_pc.instAddr() + pcOffset) & pc_mask;
    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
    issuePipelinedIfetch[tid] = fetchBufferBlockPC != fetchBufferPC[tid] &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != FTQEmpty &&
        ftq->isHeadReady(tid) &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != QuiescePending &&
        !curMacroop;
}

void
Fetch::recvReqRetry()
{
    if (retryPkt != NULL) {
        assert(cacheBlocked);
        assert(retryTid != InvalidThreadID);
        assert(fetchStatus[retryTid] == IcacheWaitRetry);

        if (icachePort.sendTimingReq(retryPkt)) {
            fetchStatus[retryTid] = IcacheWaitResponse;
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify(retryPkt->req);
            retryPkt = NULL;
            retryTid = InvalidThreadID;
            cacheBlocked = false;
        }
    } else {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
    }
}

///////////////////////////////////////
//                                   //
//  SMT FETCH POLICY MAINTAINED HERE //
//                                   //
///////////////////////////////////////
ThreadID
Fetch::getFetchingThread()
{
    if (numThreads > 1) {
        // More that one thread is not tested with decoupled Frontend.
        assert(!decoupledFrontEnd);
        switch (fetchPolicy) {
          case SMTFetchPolicy::RoundRobin:
            return roundRobin();
          case SMTFetchPolicy::IQCount:
            return iqCount();
          case SMTFetchPolicy::LSQCount:
            return lsqCount();
          case SMTFetchPolicy::Branch:
            return branchCount();
          default:
            return InvalidThreadID;
        }
    } else {
        std::list<ThreadID>::iterator thread = activeThreads->begin();
        if (thread == activeThreads->end()) {
            return InvalidThreadID;
        }

        ThreadID tid = *thread;

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == IcacheAccessComplete ||
            fetchStatus[tid] == Idle) {
            return tid;
        } else {
            return InvalidThreadID;
        }
    }
}


ThreadID
Fetch::roundRobin()
{
    std::list<ThreadID>::iterator pri_iter = priorityList.begin();
    std::list<ThreadID>::iterator end      = priorityList.end();

    ThreadID high_pri;

    while (pri_iter != end) {
        high_pri = *pri_iter;

        assert(high_pri <= numThreads);

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle) {

            priorityList.erase(pri_iter);
            priorityList.push_back(high_pri);

            return high_pri;
        }

        pri_iter++;
    }

    return InvalidThreadID;
}

ThreadID
Fetch::iqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned iqCount = fromIEW->iewInfo[tid].iqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(iqCount);
        threadMap[iqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();

    }

    return InvalidThreadID;
}

ThreadID
Fetch::lsqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned ldstqCount = fromIEW->iewInfo[tid].ldstqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(ldstqCount);
        threadMap[ldstqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();
    }

    return InvalidThreadID;
}

ThreadID
Fetch::branchCount()
{
    panic("Branch Count Fetch policy unimplemented\n");
    return InvalidThreadID;
}

void
Fetch::pipelineIcacheAccesses(ThreadID tid)
{
    if (!issuePipelinedIfetch[tid]) {
        return;
    }

    // The next PC to access.
    const PCStateBase &this_pc = *pc[tid];

    if (isRomMicroPC(this_pc.microPC())) {
        return;
    }

    Addr pcOffset = fetchOffset[tid];
    Addr fetchAddr = (this_pc.instAddr() + pcOffset) & decoder[tid]->pcMask();

    // Align the fetch PC so its at the start of a fetch buffer segment.
    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

    // Unless buffer already got the block, fetch it from icache.
    if (!(fetchBufferValid[tid] && fetchBufferBlockPC == fetchBufferPC[tid])) {
        DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
                "starting at PC %s.\n", tid, this_pc);

        fetchCacheLine(fetchAddr, tid, this_pc.instAddr());
    }
}

void
Fetch::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (stalls[tid].drain) {
        ++fetchStats.pendingDrainCycles;
        DPRINTF(Fetch, "Fetch is waiting for a drain!\n");
    } else if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitResponse) {
        cpu->fetchStats[tid]->icacheStallCycles++;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    } else if (fetchStatus[tid] == ItlbWait) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
                "finish!\n", tid);
    } else if (fetchStatus[tid] == FTQEmpty) {
        ++fetchStats.ftqStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for the BPU to fill FTQ!\n",
                tid);
    } else if (fetchStatus[tid] == TrapPending) {
        ++fetchStats.pendingTrapStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
                tid);
    } else if (fetchStatus[tid] == QuiescePending) {
        ++fetchStats.pendingQuiesceStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending quiesce "
                "instruction!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitRetry) {
        ++fetchStats.icacheWaitRetryStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                tid);
    } else if (fetchStatus[tid] == NoGoodAddr) {
            DPRINTF(Fetch, "[tid:%i] Fetch predicted non-executable address\n",
                    tid);
    } else {
        DPRINTF(Fetch, "[tid:%i] Unexpected fetch stall reason "
            "(Status: %i)\n",
            tid, fetchStatus[tid]);
    }
}

bool
Fetch::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));
    fetch->processCacheCompletion(pkt);

    return true;
}

void
Fetch::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

} // namespace o3
} // namespace gem5
