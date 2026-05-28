#include "cpu/pred/multilevel_btb.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/BTB.hh"
#include "sim/eventq.hh"
#include <algorithm>

namespace gem5::branch_prediction
{
MultiLevelBTB::MultiLevelBTBStats::MultiLevelBTBStats(statistics::Group *parent, MultiLevelBTB *btb)
    : statistics::Group(parent),
      ADD_STAT(successorCountDist, statistics::units::Count::get(),
               "Distribution of successor counts for L1 miss L2 hit branches"),
      ADD_STAT(markovDist, statistics::units::Count::get(),
               "Distribution of Markov successor distances"),
      ADD_STAT(dist1HistoryPC, statistics::units::Count::get(), "Distance (PC - LastPC) for 1-history"),
      ADD_STAT(dist1HistoryTarget, statistics::units::Count::get(), "Distance (PC - LastTarget) for 1-history"),
      ADD_STAT(dist2HistoryPC, statistics::units::Count::get(), "Distance (PC - 2ndLastPC) for 2-history"),
      ADD_STAT(dist2HistoryTarget, statistics::units::Count::get(), "Distance (PC - 2ndLastTarget) for 2-history"),
      ADD_STAT(l1MissL2Hits, statistics::units::Count::get(), "Number of L1 misses that hit in L2"),
      ADD_STAT(l3Hits, statistics::units::Count::get(), "Number of L1 misses that hit in L3"),
      ADD_STAT(uselessPrefetches, statistics::units::Count::get(), "Number of useless prefetches (L1 direct prefetch evicted)"),
      ADD_STAT(totalPrefetches, statistics::units::Count::get(), "Total number of prefetches"),
      ADD_STAT(shadowPrefetches, statistics::units::Count::get(), "Total number of shadow prefetches"),
      ADD_STAT(prefetchQueueFull, statistics::units::Count::get(), "Number of dropped prefetches due to queue full"),
      // Unified prefetch coverage
      ADD_STAT(prefetchHits, statistics::units::Count::get(), "Useful prefetches (pBuffer hit + L1 reuse)"),
      ADD_STAT(latePrefetchByPBHit, statistics::units::Count::get(), "Number of late prefetches by pBuffer hit"),
      ADD_STAT(latePrefetchByL2Hit, statistics::units::Count::get(), "Number of late prefetches by L2 hit"),
      ADD_STAT(prefetchCoverage, statistics::units::Ratio::get(), "Prefetch coverage (prefetchHits / totalPrefetches)"),
      // Separate useless rates for pBuffer and L1
      ADD_STAT(pBufferUselessRate, statistics::units::Ratio::get(), "pBuffer useless rate"),
      ADD_STAT(l1InstalledEvicted, statistics::units::Count::get(), "L1 evictions of pBuffer-installed entries without reuse"),
      ADD_STAT(l1Installed, statistics::units::Count::get(), "Entries installed from pBuffer to L1"),
      ADD_STAT(l1UselessInstalledRate, statistics::units::Ratio::get(), "L1 useless installed rate for pBuffer-installed entries"),
      ADD_STAT(numBranchesPerPrefetch, statistics::units::Count::get(), "Number of branches per prefetch"),
      ADD_STAT(prefetchDistCount, statistics::units::Count::get(), "Number of prefetches per distance"),
      ADD_STAT(prefetchDistUsed, statistics::units::Count::get(), "Number of useful prefetches per distance"),
      ADD_STAT(prefetchUsefulness, statistics::units::Ratio::get(), "Usefulness ratio per distance"),
      ADD_STAT(predMatches, statistics::units::Count::get(), "Number of prediction matches for prefetched entries"),
      ADD_STAT(predChecks, statistics::units::Count::get(), "Number of prediction checks for prefetched entries"),
      ADD_STAT(predMatchRatio, statistics::units::Ratio::get(), "Prediction match ratio for prefetched entries"),
      ADD_STAT(compressedTagChecks, statistics::units::Count::get(), "Number of compressed tag array checks"),
      ADD_STAT(compressedTagFalsePositives, statistics::units::Count::get(), "Compressed tag array false positives"),
      ADD_STAT(compressedTagFalsePositiveRate, statistics::units::Ratio::get(), "Compressed tag array false positive rate"),
      ADD_STAT(compressedTagFalseNegatives, statistics::units::Count::get(), "Compressed tag array false negatives"),
      ADD_STAT(compressedTagFalseNegativeRate, statistics::units::Ratio::get(), "Compressed tag array false negative rate"),
      ADD_STAT(compressedTagAliases, statistics::units::Count::get(), "Compressed tag array aliases"),
      ADD_STAT(shadowOverlaps, statistics::units::Count::get(), "Hit in BOTH Real and Shadow"),
      ADD_STAT(spatialOnlyHits, statistics::units::Count::get(), "Hit in Real, Miss in Shadow"),
      ADD_STAT(markovOnlyHits, statistics::units::Count::get(), "Miss in Real, Hit in Shadow"),
      ADD_STAT(uniMisses, statistics::units::Count::get(), "Miss in BOTH, but Hit in L2"),
      ADD_STAT(markovOnlyHitsByPredType, statistics::units::Count::get(), "Markov only hits broken down by predecessor type"),
      ADD_STAT(trainBitsL1Miss, statistics::units::Count::get(), "committed branch not in L1, found in L2"),
      ADD_STAT(takenPathPrefetches, statistics::units::Count::get(), " prefetches triggered via taken-path (prefetchTarget) bit"),
      ADD_STAT(notTakenPathPrefetches, statistics::units::Count::get(), " prefetches triggered via not-taken-path (prefetchThrough) bit"),
      ADD_STAT(prefetchHitsFromTaken, statistics::units::Count::get(), "Prefetch hits from taken path"),
      ADD_STAT(prefetchHitsFromNotTaken, statistics::units::Count::get(), "Prefetch hits from not-taken path"),
      ADD_STAT(callL2OrPrefetchHits, statistics::units::Count::get(), "Calls that hit in L2 or via prefetch path"),
      ADD_STAT(callFallThroughL2Only, statistics::units::Count::get(), "Calls whose fall-through branch is L2-only"),
      ADD_STAT(callFallThroughL2Ratio, statistics::units::Ratio::get(), "callFallThroughL2Only / callL2OrPrefetchHits"),
      ADD_STAT(updatesL1hits, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(updatesL2hits, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(updatesL2miss, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(updatesL3hits, statistics::units::Ratio::get(), "number of L3 updates"),
      ADD_STAT(updatesL3miss, statistics::units::Ratio::get(), "number of L3 updates"),
      ADD_STAT(pfIssued, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(pfL2LookupHit, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(pfL2LookupMiss, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(mkHits, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(pfTriggerCall, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(pfTriggerBwExit, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(pfTriggerFwExit, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(pfTriggerCondAlt, statistics::units::Ratio::get(), "number of L2 updates"),
      ADD_STAT(skipDuetoDemand, statistics::units::Count::get(),
               "Prefetch entries dropped: demand access to same PC"),
      ADD_STAT(skipDuetoPresence, statistics::units::Count::get(),
               "Prefetch entries skipped: PC already present in L1 or pBuffer"),
      ADD_STAT(enqueueDeferred, statistics::units::Count::get(),
               "Total deferred prefetch entries processed (denominator for skip ratios)"),
      ADD_STAT(skipDuetoDemandRatio, statistics::units::Ratio::get(),
               "skipDuetoDemand / enqueueDeferred"),
      ADD_STAT(skipDuetoPresenceRatio, statistics::units::Ratio::get(),
               "skipDuetoPresence / enqueueDeferred"),
      ADD_STAT(chainEndEvicted, statistics::units::Count::get(),
               "Chain ended early: slot evicted by newer chain"),
      ADD_STAT(chainEndDemand, statistics::units::Count::get(),
               "Chain ended early: killed by demand access to same PC"),
      ADD_STAT(chainEndPresence, statistics::units::Count::get(),
               "Chain ended early: killed by L1/PB presence"),
      ADD_STAT(chainEndL2Miss, statistics::units::Count::get(),
               "Chain ended early: L2 miss for prefetch target"),
      ADD_STAT(chainEndRetFilter, statistics::units::Count::get(),
               "Chain ended early: filtered by limitRet (Return type)"),
      ADD_STAT(chainEndNonEntry, statistics::units::Count::get(),
               "Might be ended by non-BTBEntry blocks"),
      ADD_STAT(chainEndDepthExhaust, statistics::units::Count::get(),
               "Chain ended: remaining depth reached 0"),
      ADD_STAT(prefetchesPerTrigger, statistics::units::Count::get(), "Number of prefetches generated per trigger"),
      ADD_STAT(parallelChains, statistics::units::Count::get(), "Number of parallel prefetch chains active"),

      btb(btb)
{
    using namespace statistics;
    successorCountDist.init(0).flags(total | pdf);
    markovDist.init(0).flags(total | pdf);
    dist1HistoryPC.init(0).flags(total | pdf);
    dist1HistoryTarget.init(0).flags(total | pdf);
    dist2HistoryPC.init(0).flags(total | pdf);
    dist2HistoryTarget.init(0).flags(total | pdf);

    l1MissL2Hits.flags(total);
    l3Hits.flags(total);
    takenPathPrefetches.flags(total);
    notTakenPathPrefetches.flags(total);
    prefetchHitsFromTaken.init(enums::Num_BranchType).flags(total | pdf);
    prefetchHitsFromNotTaken.init(enums::Num_BranchType).flags(total | pdf);
    uselessPrefetches.init(enums::Num_BranchType).flags(total | pdf);
    totalPrefetches.flags(total);
    shadowPrefetches.flags(total);
    prefetchQueueFull.flags(total);
    prefetchDistCount.init(16);
    prefetchDistUsed.init(16);
    prefetchHits.init(enums::Num_BranchType).flags(total | pdf);
    prefetchUsefulness = prefetchDistUsed / prefetchDistCount;
    prefetchUsefulness.precision(3);
    predMatches.init(15);
    predChecks.init(15);

    predMatchRatio = predMatches / predChecks;

    latePrefetchByPBHit.init(5).flags(total | pdf);
    latePrefetchByL2Hit.init(5).flags(total | pdf);
    // Policy 11 Shadow Stats
    shadowOverlaps.init(enums::Num_BranchType).flags(total | pdf);
    spatialOnlyHits.init(enums::Num_BranchType).flags(total | pdf);
    markovOnlyHits.init(enums::Num_BranchType).flags(total | pdf);
    uniMisses.init(enums::Num_BranchType).flags(total | pdf);

    markovOnlyHitsByPredType.init(enums::Num_BranchType).flags(total | pdf);
    for (int i = 0; i < enums::Num_BranchType; i++) {
        uselessPrefetches.subname(i, enums::BranchTypeStrings[i]);
        prefetchHits.subname(i, enums::BranchTypeStrings[i]);
        prefetchHitsFromTaken.subname(i, enums::BranchTypeStrings[i]);
        prefetchHitsFromNotTaken.subname(i, enums::BranchTypeStrings[i]);
        shadowOverlaps.subname(i, enums::BranchTypeStrings[i]);
        spatialOnlyHits.subname(i, enums::BranchTypeStrings[i]);
        markovOnlyHits.subname(i, enums::BranchTypeStrings[i]);
        uniMisses.subname(i, enums::BranchTypeStrings[i]);
        markovOnlyHitsByPredType.subname(i, enums::BranchTypeStrings[i]);
    }
    // Unified prefetch coverage formula
    prefetchCoverage = sum(prefetchHits) / (sum(prefetchHits) + l1MissL2Hits);
    prefetchCoverage.precision(3);

    // pBuffer useless rate
    pBufferUselessRate = sum(uselessPrefetches) / totalPrefetches;
    pBufferUselessRate.precision(3);

    // L1 useless prefetch rate (for pBuffer-installed entries)
    l1UselessInstalledRate = l1InstalledEvicted / l1Installed;
    l1UselessInstalledRate.precision(3);
    callL2OrPrefetchHits.flags(total);
    callFallThroughL2Only.flags(total);
    callFallThroughL2Ratio = callFallThroughL2Only / callL2OrPrefetchHits;
    callFallThroughL2Ratio.precision(3);

    compressedTagChecks.flags(total);
    compressedTagFalsePositives.flags(total);
    compressedTagFalsePositiveRate = compressedTagFalsePositives / compressedTagChecks;
    compressedTagFalsePositiveRate.precision(4);

    compressedTagFalseNegatives.flags(total);
    compressedTagFalseNegativeRate = compressedTagFalseNegatives / compressedTagChecks;
    compressedTagFalseNegativeRate.precision(4);

    compressedTagAliases.flags(total);

    skipDuetoDemandRatio = skipDuetoDemand / enqueueDeferred;
    skipDuetoDemandRatio.precision(4);
    skipDuetoPresenceRatio = skipDuetoPresence / enqueueDeferred;
    skipDuetoPresenceRatio.precision(4);

    numBranchesPerPrefetch.init(0, 64, 1);
    prefetchesPerTrigger.init(0, 64, 1); // Buckets from 0 to 512 with step 4
    parallelChains.init(0, 32, 1);       // Buckets from 0 to 128 with step 1
}

void
MultiLevelBTB::MultiLevelBTBStats::preDumpStats()
{
    statistics::Group::preDumpStats();

    for (const auto& pair : btb->markovSuccessors) {
        size_t count = pair.second.size();
        successorCountDist.sample(count);
    }
    btb->markovSuccessors.clear();
    std::fill(btb->prevBranchPC.begin(), btb->prevBranchPC.end(), 0);
    std::fill(btb->shadowPrevBranchPC.begin(), btb->shadowPrevBranchPC.end(), 0);
    for (auto &info : btb->prevCommitBlockInfo) {
        info = PrevCommitBlockInfo();
    }
}

MultiLevelBTB::MultiLevelBTB(const MultiLevelBTBParams &p)
    : BranchTargetBuffer(p),
      l1btb("l1BTB", p.l1NumEntries, p.l1Associativity,
            p.l1ReplPolicy, p.l1IndexingPolicy, BTBEntry(genTagExtractor(p.l1IndexingPolicy))),
      pBuffer("prefetchBuffer", p.pBufferSize, 1, p.pBufferReplPolicy, p.pBufferIndexingPolicy, BTBEntry(genTagExtractor(p.pBufferIndexingPolicy))),
      l2btb("l2BTB", p.l2NumEntries, p.l2Associativity,
            p.l2ReplPolicy, p.l2IndexingPolicy,
            BTBEntry(genTagExtractor(p.l2IndexingPolicy))),
      l3btb("l3BTB", p.l3NumEntries, p.l3Associativity,
            p.l3ReplPolicy, p.l3IndexingPolicy,
            BTBEntry(genTagExtractor(p.l3IndexingPolicy))),
      l1Latency(p.l1Latency),
      l2Latency(p.l2Latency),
      l3Latency(p.l3Latency),
      enableL3(p.enableL3),
      minInstSize(p.minInstSize),
      l1PrefetchPolicy(p.l1PrefetchPolicy),
      trainBitsOnLookup(p.trainBitsOnLookup),
      trainBitsOnCommit(p.trainBitsOnCommit),
      prefetchOnlyCB(p.prefetchOnlyCB),
      prefetchOnlyUB(p.prefetchOnlyUB),
      togetherArrive(p.togetherArrive),
      prefetchOnL1Hit(p.prefetchOnL1Hit),
      prefetchOnPrefetchHit(p.prefetchOnPrefetchHit),
      cleanBitsOnL1Promotion(p.cleanBitsOnL1Promotion),
      noPrefetchLatency(p.noPrefetchLatency),
      prefetchDepth(p.prefetchDepth),
      maxChainTrackerEntries(p.maxChainTrackerEntries),
      depthOnlyCall(p.depthOnlyCall),
      prefetchOnlyForward(p.prefetchOnlyForward),
      finalMarkov(p.finalMarkov),
      prefetchAllMarkovSuccessors(p.prefetchAllMarkovSuccessors),
      limitRet(p.limitRet),
      markovUseRecency(p.markovUseRecency),
      updateDirOnlyL1(p.updateDirOnlyL1),
      newPBits(p.newPBits),
      onlyCall(p.onlyCall),
      onlyCallAndBackward(p.onlyCallAndBackward),
      callFallthrough(p.callFallthrough),
      forwardLoopExit(p.forwardLoopExit),
      backwardLoopExit(p.backwardLoopExit),
      allConditional(p.allConditional),
      prefetchFwExitOnL1Hit(p.prefetchFwExitOnL1Hit),
      useCompressedTagFilter(p.useCompressedTagFilter),
      multilevelstats(this, this),
      l1MissL2HitHistory(p.numThreads),
      prevBranchPC(p.numThreads, 0),
      shadowPrevBranchPC(p.numThreads, 0),
      prevBlockInfo(p.numThreads),
      prevCommitBlockInfo(p.numThreads),
      shadowL1BTB("shadowL1BTB", p.l1NumEntries, p.l1Associativity,
            p.l1ReplPolicy, p.l1IndexingPolicy, BTBEntry(genTagExtractor(p.l1IndexingPolicy))),
      shadowPBuffer("shadowPrefetchBuffer", p.pBufferSize, 1, p.pBufferReplPolicy, p.pBufferIndexingPolicy, BTBEntry(genTagExtractor(p.pBufferIndexingPolicy))),
      l1CompressedTags("l1CompressedTags", p.l1NumEntries, p.l1Associativity,
            p.compressedTagReplPolicy, p.compressedTagIndexingPolicy,
            BTBEntry(genTagExtractor(p.compressedTagIndexingPolicy))),
      pbCompressedTags("pbCompressedTags", p.pBufferSize, 8,
            p.pbCompressedTagReplPolicy, p.pbCompressedTagIndexingPolicy,
            BTBEntry(genTagExtractor(p.pbCompressedTagIndexingPolicy))),
      pfqEvent([this]{ processDeferredPrefetchQueue(); }, name(),
               false, Event::CPU_Tick_Pri + 1)
{
    DPRINTF(BTB, "MultiLevelBTB: Creating L1(%d entries, %d cycles) + L2(%d entries, %d cycles) + L3(%d entries, %d cycles)\n",
            p.l1NumEntries, p.l1Latency, p.l2NumEntries, p.l2Latency, p.l3NumEntries, p.l3Latency);

    chainTable.resize(maxChainTrackerEntries);

    
}

void
MultiLevelBTB::startup()
{
    schedule(pfqEvent, clockEdge(Cycles(1)));
}

void
MultiLevelBTB::memInvalidate()
{
    l1btb.clear();
    l2btb.clear();
    if (enableL3) l3btb.clear();
    pBuffer.clear();
    shadowL1BTB.clear();
    shadowPBuffer.clear();
    l1CompressedTags.clear();
    pbCompressedTags.clear();
    deferredPrefetchQueue.clear();
    for (auto& entry : chainTable) {
        entry.chainId = 0;
        entry.remainingPrefetches = 0;
        entry.lastEndReason = ChainEndReason::None;
    }
}

void
MultiLevelBTB::enqueuePrefetch(Addr pc, ThreadID tid, BTBEntry *l2_entry,
                                Cycles arrivalCycle, bool toL1,
                                bool triggeredByPBHit, uint8_t pfDistance,
                                bool takenPrefetched, BranchType triggerType)
{
    // Skip if already in pBuffer or L1
    if (l1ApproxContains(pc, tid) || pbApproxContains(pc, tid))
        return;

    multilevelstats.pfIssued++;
    multilevelstats.totalPrefetches++;
    if (usesPrefetchBitPolicy()) {
        if (takenPrefetched) {
            multilevelstats.takenPathPrefetches++;
        } else {
            multilevelstats.notTakenPathPrefetches++;
        }
    }

    BTBEntry *victim = pBuffer.findVictim({pc, tid});
    if (victim->isPrefetched()) {
        multilevelstats.uselessPrefetches[victim->getPrefetchTriggerType()]++;
    }
    pBuffer.insertEntry({pc, tid}, victim);
    pbCompressedTagSync(pc, tid, TagAction::Insert);

    victim->update(*l2_entry->target, l2_entry->inst);
    victim->copyDir(*l2_entry);
    victim->setTimestamp(arrivalCycle);  // timestamp tracks arrival cycle
    victim->setPrefetched(true);
    victim->setTriggeredByPBHit(triggeredByPBHit);
    victim->setPrefetchTarget(l2_entry->getPrefetchTarget());
    victim->setPrefetchThrough(l2_entry->getPrefetchThrough());
    victim->setPrefetchDistance(pfDistance);
    victim->setPrefetchTriggerType(triggerType);
    if (usesPrefetchBitPolicy() && takenPrefetched) {
        victim->setTakenPrefetched(true);
    }
}

bool
MultiLevelBTB::valid(ThreadID tid, Addr instPC)
{
    BTBEntry *l1_entry = l1btb.findEntry({instPC, tid});
    if (l1_entry != nullptr) {
        DPRINTF(BTB, "L1 BTB valid for PC %#x\n", instPC);
        return true;
    }
    BTBEntry *pB_entry = pBuffer.findEntry({instPC, tid});
    if(pB_entry != nullptr) {
        DPRINTF(BTB, "Prefetch buffer valid for PC %#x\n", instPC);
        return true;
    }
    BTBEntry *l2_entry = l2btb.findEntry({instPC, tid});
    if(l2_entry != nullptr) {
        DPRINTF(BTB, "L2 BTB valid for PC %#x\n", instPC);
        return true;
    }
    if (enableL3) {
        BTBEntry *l3_entry = l3btb.findEntry({instPC, tid});
        if(l3_entry != nullptr) {
            DPRINTF(BTB, "L3 BTB valid for PC %#x\n", instPC);
            return true;
        }
    }
    return false;
}

const PCStateBase *
MultiLevelBTB::lookup(ThreadID tid, Addr instPC, BranchType type)
{
    BTBLookupResult result = lookupWithLatency(tid, instPC, type);
    return result.target;
}

BTBLookupResult
MultiLevelBTB::lookupWithLatency(ThreadID tid, Addr instPC, BranchType type,
                                 bool taken, Addr blockStartAddr, bool basePrediction)
{
    DPRINTF(BTB, "%s(pc=%#x)\n", __func__, instPC);
    recordDemandLookup(instPC);


    stats.lookups[type]++;

    if (l1PrefetchPolicy == 11) {
        performShadowLookup(tid, instPC, type);
    }

    // trainBitsOnLoopup: Compare current block's start address with prev entry's target/fallThrough
    // if (usesBlockTrainPolicy() && blockStartAddr != 0) {
    if (trainBitsOnLookup && blockStartAddr != 0) {
        auto &prev = prevBlockInfo[tid];
        if (prev.valid) {
            BTBEntry *prevL1 = l1btb.findEntry({prev.branchPC, tid});
            if (prevL1) {
                if (blockStartAddr == prev.target) {
                    prevL1->setPrefetchTarget(true);
                }
                if (blockStartAddr == prev.fallThrough) {
                    prevL1->setPrefetchThrough(true);
                }
            }
        }
    }

    // ==========================================================================
    // Step 1: L1 BTB lookup
    // ==========================================================================
    BTBEntry *l1_entry = l1btb.accessEntry({instPC, tid});
    l1CompressedTagSync(instPC, tid, TagAction::Hit);
    if (l1_entry != nullptr) {
        // trainBitsOnLookup: record current block info for next training iteration
        if (trainBitsOnLookup && blockStartAddr != 0)
            recordPrevBlockInfo(tid, instPC, l1_entry->target->instAddr());
        return handleL1Hit(tid, instPC, l1_entry, type, taken, basePrediction);
    }

    // ==========================================================================
    // Step 2: pBuffer lookup (includes both arrived and in-flight prefetches)
    // ==========================================================================
    if (l1PrefetchPolicy == 4 || l1PrefetchPolicy == 6 || l1PrefetchPolicy == 7 ||
        l1PrefetchPolicy == 8 || l1PrefetchPolicy == 9 || l1PrefetchPolicy == 10 ||
        l1PrefetchPolicy == 11 || finalMarkov || trainBitsOnLookup ||
        trainBitsOnCommit) {
        BTBEntry *pB_entry = pBuffer.accessEntry({instPC, tid});
        if (pB_entry != nullptr) {
            if (trainBitsOnLookup && blockStartAddr != 0)
                recordPrevBlockInfo(tid, instPC, pB_entry->target->instAddr());
            return handlePBufferHit(tid, instPC, pB_entry, type, taken, basePrediction);
        }
    }

    // ==========================================================================
    // Step 3: L2 BTB lookup
    // ==========================================================================
    BTBEntry *l2_entry = l2btb.accessEntry({instPC, tid});
    if (l2_entry != nullptr) {
        // trainBitsOnLookup: record current block info (from L2 entry)
        if (trainBitsOnLookup && blockStartAddr != 0)
            recordPrevBlockInfo(tid, instPC, l2_entry->target->instAddr());
        return handleL2Hit(tid, instPC, l2_entry, type, taken);
    }

    // ==========================================================================
    // Step 4: L3 BTB lookup
    // ==========================================================================
    if (enableL3) {
        BTBEntry *l3_entry = l3btb.accessEntry({instPC, tid});
        if (l3_entry != nullptr) {
            if (trainBitsOnLookup && blockStartAddr != 0)
                recordPrevBlockInfo(tid, instPC, l3_entry->target->instAddr());
            return handleL3Hit(tid, instPC, l3_entry, type, taken);
        }
    }

    // The progrem will never get here, otherwise there is bug.
    panic("There is bug in bpu->BTBValid(tid, br_addr) from bac.cc");
}

const StaticInstPtr
MultiLevelBTB::getInst(ThreadID tid, Addr instPC)
{
    //For this implementation, L2 is not strictly inclusive of L1, so we need to check both
    BTBEntry *l1_entry = l1btb.findEntry({instPC, tid});
    if (l1_entry) {
        return l1_entry->inst;
    }
    BTBEntry *pB_entry = pBuffer.findEntry({instPC, tid});
    if (pB_entry) {
        return pB_entry->inst;
    }
    BTBEntry *l2_entry = l2btb.findEntry({instPC, tid});
    if (l2_entry) {
        return l2_entry->inst;
    }
    if (enableL3) {
        BTBEntry *l3_entry = l3btb.findEntry({instPC, tid});
        if (l3_entry) {
            return l3_entry->inst;
        }
    }
    return nullptr;
}

Addr
MultiLevelBTB::lookupL1(ThreadID tid, Addr inst_pc)
{
    BTBEntry *l1_entry = l1btb.findEntry({inst_pc, tid});
    if (l1_entry) {
        return l1_entry->target->instAddr();
    }
    return MaxAddr;
}

void
MultiLevelBTB::updateDirection(ThreadID tid, Addr inst_pc, bool taken)
{
    BTBEntry *entry = l1btb.findEntry({inst_pc, tid});
    if (entry) {
        entry->updateDir(taken);
    }
    if (updateDirOnlyL1)
        return;

    entry = l2btb.findEntry({inst_pc, tid});
    if (entry) {
        entry->updateDir(taken);
    }
    entry = pBuffer.findEntry({inst_pc, tid});
    if (entry) {
        entry->updateDir(taken);
    }
    if (enableL3) {
        entry = l3btb.findEntry({inst_pc, tid});
        if (entry) {
            entry->updateDir(taken);
        }
    }
}


void
MultiLevelBTB::update(ThreadID tid, Addr instPC,
                      const PCStateBase &target,
                      BranchType type, StaticInstPtr inst)
{
    stats.updates[type]++;

    DPRINTF(BTB, "%s(pc=%#x, tgt=%#x)\n", __func__, instPC, target.instAddr());

    // L1 update -----------------------
    BTBEntry* entry = l1btb.findEntry({instPC, tid});
    if (!entry) {

        // L1 miss find victim
        entry = freeUpL1Entry(tid, instPC);
        if (usesPrefetchBitPolicy()) {
            entry->setPrefetchTarget(true);
            entry->setPrefetchThrough(false);
        }
    }
    l1CompressedTagSync(instPC, tid, TagAction::Insert);
    entry->update(target, inst);
    l1btb.accessEntry(entry);

    // L2 will be updated on L1 evictions.
    return;
}

//=============================================================================
// handleL1Hit: Handle L1 BTB hit
//
// This function handles all L1 BTB hit scenarios:
// - Normal L1 hit (entry was not prefetched)
// - Prefetched entry hit: update stats
// - Policy 3 (NextRegionOnL1Hit): trigger next-region prefetch on prefetch hit
// - Prediction match checking for prefetched entries
//=============================================================================
BTBLookupResult
MultiLevelBTB::handleL1Hit(ThreadID tid, Addr instPC, BTBEntry *l1_entry,
                           BranchType type, bool taken, bool basePrediction)
{
    bool isPrefetchHit = false;
    uint8_t prefetchDistance = 0;

    // Case 1: Hit on a prefetched entry
    if (l1_entry->isPrefetched()) {
        isPrefetchHit = true;
        multilevelstats.prefetchHits[l1_entry->getPrefetchTriggerType()]++;

        // Track prefetch distance statistics (only for spatial prefetch policies 1-4)
        if ((l1PrefetchPolicy <= 4 || l1PrefetchPolicy == 11) && l1_entry->getPrefetchDistance() < 16) {
            prefetchDistance = l1_entry->getPrefetchDistance();
            multilevelstats.prefetchDistUsed[prefetchDistance]++;
        }
        l1_entry->setPrefetched(false);

        // Policy 5 (Markov to L1): Continue prefetch chain on L1 prefetch hit
        // Prefetch the most frequent successor to L1
        if (l1PrefetchPolicy == 5) {
            prefetchMarkovSuccessor(tid, instPC, true, 1, true, Cycles(0),
                                    type);  // Prefetch 1 to L1
        }

    // Case 2: Hit on entry installed from pBuffer (Policy 4)
    } else if (l1_entry->isFromPBuffer()) {
        l1_entry->setFromPBuffer(false);  // Only count first reuse
    }

    // Prefetch-bit prefetcher: trigger prefetch on L1 hit based on prefetch bits
    if (bbMap_) {
        Addr targetAddr = l1_entry->target->instAddr();
        Addr fallThrough = instPC + minInstSize;
        auto baseLatency = Cycles(0);

        bool doPfTarget = l1_entry->getPrefetchTarget();
        bool doPfThrough = l1_entry->getPrefetchThrough();

        bool isBackward = (l1_entry->target->instAddr() < instPC);
        applyNewPBitsLogic(type, basePrediction, isBackward, doPfTarget, doPfThrough, L1Hit);
        int effectiveDepth = (depthOnlyCall && !isCall(type)) ? 1 : prefetchDepth;
        prevBwBranch.is_bw = isBackward;
        prevBwBranch.is_l2_miss = false;

        if (doPfTarget || doPfThrough) {
            uint64_t currentChainId = nextChainId++;

            if (doPfTarget) {
                prefetchViaBBMap(tid, targetAddr, true, true, effectiveDepth, baseLatency, type, currentChainId, true);
                baseLatency = baseLatency + Cycles(1);
            }
            if (doPfThrough) {
                prefetchViaBBMap(tid, fallThrough, false, true, effectiveDepth, baseLatency, type, currentChainId, true);
            }
        }
    }

    if (prefetchOnL1Hit && finalMarkov) {
        unsigned numSucc = (finalMarkov && prefetchAllMarkovSuccessors) ? 100 : 1;
        if (limitRet && finalMarkov && prefetchAllMarkovSuccessors && type == BranchType::Return) {
            numSucc = 2;
        }
        prefetchMarkovSuccessor(tid, instPC, false, numSucc, true, Cycles(0), type, prefetchDepth);
    }

    DPRINTF(BTB, "L1 BTB hit for PC %#x, latency=%d cycles\n",
            instPC, l1Latency);

    // Check prediction match for prefetched entries
    bool predMatch = false;
    // if (isPrefetchHit && cPred) {
    //     multilevelstats.predChecks[prefetchDistance]++;
    //     if (l1_entry->getPredTaken() == taken) {
    //         if (prefetchDistance == 0) {
    //             predMatch = true;
    //         }
    //         multilevelstats.predMatches[prefetchDistance]++;
    //     }
    // }

    return BTBLookupResult(l1_entry->target.get(), l1Latency,
                           true, false, false, false, isPrefetchHit, predMatch, l1_entry->getDir(), l1_entry->getPrefetchTriggerType());
}


BTBLookupResult
MultiLevelBTB::handlePBufferHit(ThreadID tid, Addr instPC,
                                BTBEntry *pB_entry, BranchType type, bool taken, bool basePrediction)
{
    bool isInFlight = !noPrefetchLatency &&
                      pB_entry->getTimestamp() > Cycles(0) &&
                      curCycle() < pB_entry->getTimestamp();

    Cycles remainingTime = Cycles(0);
    Cycles coveredCycle = Cycles(0);

    if (isInFlight) {
        remainingTime = pB_entry->getTimestamp() - curCycle();
        coveredCycle = l2Latency - remainingTime;
    }

    BranchType triggerType = pB_entry->getPrefetchTriggerType();
    if (pB_entry->isPrefetched()) {
        multilevelstats.prefetchHits[triggerType]++;
        if (usesPrefetchBitPolicy()) {
            if (pB_entry->isTakenPrefetched()) {
                multilevelstats.prefetchHitsFromTaken[triggerType]++;
            } else {
                multilevelstats.prefetchHitsFromNotTaken[triggerType]++;
            }
        }
        pB_entry->setPrefetched(false);
        pB_entry->setTakenPrefetched(false);

        if (isInFlight) {
            if (pB_entry->isTriggeredByPBHit()) {
                multilevelstats.latePrefetchByPBHit[coveredCycle]++;
            } else {
                multilevelstats.latePrefetchByL2Hit[coveredCycle]++;
            }
        } else {
            if (pB_entry->isTriggeredByPBHit()) {
                multilevelstats.latePrefetchByPBHit[4]++;
            } else {
                multilevelstats.latePrefetchByL2Hit[4]++;
            }
        }
        pB_entry->setTriggeredByPBHit(false);
    }

    // Track prefetch distance statistics
    uint8_t prefetchDistance = pB_entry->getPrefetchDistance();
    if ((l1PrefetchPolicy == 4 || l1PrefetchPolicy == 11) && prefetchDistance < 16) {
        multilevelstats.prefetchDistUsed[prefetchDistance]++;
    }

    // -------------------------------------------------------------------------
    // Promote entry from pBuffer to L1
    // -------------------------------------------------------------------------
    BTBEntry *l1_victim = freeUpL1Entry(tid, instPC);
    l1_victim->update(*pB_entry);
    l1CompressedTagSync(instPC, tid, TagAction::Insert);

    if (l1PrefetchPolicy == 4 || l1PrefetchPolicy == 11) {
        l1_victim->setPrefetchDistance(prefetchDistance);
    }
    l1_victim->setPredTaken(pB_entry->getPredTaken());
    l1_victim->setPrefetched(false);

    if (!cleanBitsOnL1Promotion) {
        l1_victim->setPrefetchThrough(pB_entry->getPrefetchThrough());
        l1_victim->setPrefetchTarget(pB_entry->getPrefetchTarget());
    } else {
        l1_victim->setPrefetchThrough(false);
        l1_victim->setPrefetchTarget(false);
    }

    multilevelstats.l1Installed++;

    if (isCall(type) && bbMap_) {
        multilevelstats.callL2OrPrefetchHits++;
        Addr ft = instPC + minInstSize;
        auto it = bbMap_->find(ft);
        if (it != bbMap_->end()) {
            Addr ftPC = it->second;
            if (!l1btb.findEntry({ftPC, tid}) &&
                !pBuffer.findEntry({ftPC, tid}) &&
                l2btb.findEntry({ftPC, tid})) {
                multilevelstats.callFallThroughL2Only++;
            }
        }
    }

    if (bbMap_) {
        Addr targetAddr = pB_entry->target->instAddr();
        Addr fallThrough = instPC + minInstSize;
        bool doPfTarget = pB_entry->getPrefetchTarget();
        bool doPfThrough = pB_entry->getPrefetchThrough();

        bool isBackward = (pB_entry->target->instAddr() < instPC);
        applyNewPBitsLogic(type, basePrediction, isBackward, doPfTarget, doPfThrough, PBHit);

        int effectiveDepth = (depthOnlyCall && !isCall(type)) ? 1 : prefetchDepth;
        prevBwBranch.is_bw = isBackward;
        prevBwBranch.is_l2_miss = true;

        bool triggeredByPBHit = !isInFlight || coveredCycle > Cycles(0);
        auto baseLatency = remainingTime;
        if (doPfTarget || doPfThrough) {
            uint64_t currentChainId = nextChainId++;
            
            if (doPfTarget) {
                prefetchViaBBMap(tid, targetAddr, true, triggeredByPBHit,
                    effectiveDepth, baseLatency, type, currentChainId, true);
                baseLatency = baseLatency + Cycles(1);
            }
            if (doPfThrough) {
                prefetchViaBBMap(tid, fallThrough, false, triggeredByPBHit,
                    effectiveDepth, baseLatency, type, currentChainId, true);
            }
        }
    } 
    pBuffer.invalidate(pB_entry);
    pbCompressedTagSync(instPC, tid, TagAction::Invalidate);

    // -------------------------------------------------------------------------
    // Markov prefetch chain
    // -------------------------------------------------------------------------
    if (l1PrefetchPolicy == 6 || l1PrefetchPolicy == 7 ||
        l1PrefetchPolicy == 8 || finalMarkov) {
        unsigned numSucc = (finalMarkov && prefetchAllMarkovSuccessors) ? 100 : 1;
        if (limitRet && finalMarkov && prefetchAllMarkovSuccessors && type == BranchType::Return) {
            numSucc = 2;
        }
        prefetchMarkovSuccessor(tid, instPC, false, numSucc, true,
                                remainingTime, type, prefetchDepth);
    } else if (l1PrefetchPolicy == 9) {
        prefetchMarkovSuccessor(tid, instPC, false, 2, true,
                                remainingTime, type);
    } else if (l1PrefetchPolicy == 10) {
        prefetchMarkovSuccessor(tid, instPC, false, 3, true,
                                remainingTime, type);
    } else if (l1PrefetchPolicy == 5) {
        prefetchMarkovSuccessor(tid, instPC, true, 1, true,
                                remainingTime, type);
    }

    // -------------------------------------------------------------------------
    // Return result
    // -------------------------------------------------------------------------
    DPRINTF(BTB, "pBuffer hit for PC %#x (inFlight=%d, covered=%d), promoted to L1\n",
            instPC, isInFlight, (int)coveredCycle);

    return BTBLookupResult(l1_victim->target.get(), remainingTime,
                           false, true, false, false, true, false, l1_victim->getDir(), triggerType);
}

//=============================================================================
// handleL2Hit: Handle L2 BTB hit
//
// This function handles L2 BTB hits:
// - Inserts the entry into L1
// - Performs prefetching based on the active policy:
//   - Policy 1: Prefetch next 128 bytes to L1
//   - Policy 2/3: Prefetch to end of region + 128 bytes to L1
//   - Policy 4: Prefetch to pBuffer
// - Tracks successor relationships for Markov prefetcher exploration
//=============================================================================
BTBLookupResult
MultiLevelBTB::handleL2Hit(ThreadID tid, Addr instPC, BTBEntry *l2_entry,
                           BranchType type, bool taken)
{
    multilevelstats.l1MissL2Hits++;
    if (bbMap_ &&
        (type == BranchType::CallDirect || type == BranchType::CallIndirect)) {
        multilevelstats.callL2OrPrefetchHits++;
        Addr ft = instPC + minInstSize;
        auto it = bbMap_->find(ft);
        if (it != bbMap_->end()) {
            Addr ftPC = it->second;
            if (!l1btb.findEntry({ftPC, tid}) &&
                !pBuffer.findEntry({ftPC, tid}) &&
                l2btb.findEntry({ftPC, tid})) {
                multilevelstats.callFallThroughL2Only++;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Insert entry into L1
    // -------------------------------------------------------------------------

    // Keep hierarchy non-redundant: on L2 hit, migrate the entry to L1.
    // Snapshot first because freeUpL1Entry() may overwirte l2_entry if l2_entry is invalidated.
    BTBEntry l2_snapshot(*l2_entry);
    DPRINTF(BTB, "%s(pc=%#x) -> L2[pc=%#x tgt=%#x], migrate to L1\n", __func__, instPC,
            l2_snapshot.getBranchAddr(), l2_snapshot.target->instAddr());
    l2btb.invalidate(l2_entry);

    BTBEntry *l1_victim = freeUpL1Entry(tid, instPC);
    assert(instPC == l2_snapshot.getBranchAddr()); // Ensure the write back has not modified the L2 entry.

    l1_victim->update(l2_snapshot);
    DPRINTF(BTB, "L2 BTB hit for PC %#x, latency=%d cycles, insert in L1\n",
            instPC, l2Latency);

    // l1_victim = l1btb.findVictim({instPC, tid});
    // writebackToL2(tid, l1_victim);
    // // @Yongjie, this is probably not working anymore. It should now be moved to the `freeUpL1Entry` function
    // if (l1_victim->isPrefetched()) {
    //     multilevelstats.uselessPrefetches[l1_victim->getPrefetchTriggerType()]++;
    //     l1_victim->setPrefetched(false);
    // } else if (l1_victim->isFromPBuffer()) {
    //     multilevelstats.l1InstalledEvicted++;
    //     l1_victim->setFromPBuffer(false);
    // }
    // l1btb.insertEntry({instPC, tid}, l1_victim);
    // l1_victim->update(*l2_entry->target, l2_entry->inst);
    // l1_victim->copyDir(*l2_entry);

    l1CompressedTagSync(instPC, tid, TagAction::Insert);
    // cleanBitsOnL1Promotion: reset prefetch bits on L2->L1 demand fill
    if (!cleanBitsOnL1Promotion) {
        l1_victim->setPrefetchThrough(l2_snapshot.getPrefetchThrough());
        l1_victim->setPrefetchTarget(l2_snapshot.getPrefetchTarget());
    } else {
        l1_victim->setPrefetchThrough(false);
        l1_victim->setPrefetchTarget(false);
    }

    // -------------------------------------------------------------------------
    // Determine if we should prefetch (forward-only filtering)
    // -------------------------------------------------------------------------
    bool doPrefetch = true;
    if (prefetchOnlyForward) {
        Addr targetAddr = l2_snapshot.target->instAddr();
        bool isBackward = (targetAddr < instPC);
        if (type == BranchType::DirectCond || type == BranchType::IndirectCond) {
            if (isBackward && taken) {
                doPrefetch = false;
            }
        } else {
            doPrefetch = !isBackward;
        }
    }

    // -------------------------------------------------------------------------
    // Perform prefetching based on policy (Policy 1-4 only, not 5/6)
    // -------------------------------------------------------------------------
    if (((l1PrefetchPolicy > 0 && l1PrefetchPolicy <= 4) || l1PrefetchPolicy == 11) && doPrefetch) {
        Addr endOffset = 128;

        // Policy 2/3: Extend to end of region + 128 bytes
        if (l1PrefetchPolicy > 1 && l1PrefetchPolicy != 4 && l1PrefetchPolicy != 11) {
            Addr regionEnd = (instPC & ~127) + 128;
            endOffset = regionEnd - instPC + 128;
        }

        // All spatial blocks arrive together at curCycle + l2Latency
        Cycles arrival = curCycle() + l2Latency + l2Latency;
        bool toL1 = !(l1PrefetchPolicy == 4 || l1PrefetchPolicy == 11);

        size_t numBranches = 0;
        for (Addr offset = minInstSize; offset <= endOffset; offset += minInstSize) {
            Addr pfAddr = instPC + offset;
            BTBEntry *l2_pf = l2btb.findEntry({pfAddr, tid});
            if (!l2_pf) continue;

            if (l1ApproxContains(pfAddr, tid)) continue;

            if (!toL1 && pbApproxContains(pfAddr, tid)) continue;

            numBranches++;

            uint8_t dist = (numBranches < 16) ? numBranches : 0;
            if (numBranches < 16)
                multilevelstats.prefetchDistCount[numBranches]++;

            enqueuePrefetch(pfAddr, tid, l2_pf, arrival, toL1,
                            false, dist, false, type);
        }
        multilevelstats.numBranchesPerPrefetch.sample(numBranches);
    }

    // -------------------------------------------------------------------------
    // Markov prefetcher: Learn successor relationships with frequency
    // Policy 6: Train on all branches (speculative - at lookup time)
    // Policy 7/8: Don't train here (train only on commit for committed branches)
    // -------------------------------------------------------------------------
    // Policy 6: Train on all branches (speculative - at lookup time)
    // Policy 7/8: Don't train here (train only on commit for committed branches)
    // -------------------------------------------------------------------------
    if (l1PrefetchPolicy == 6 || l1PrefetchPolicy == 5) {
        // Policy 6: Train on all branches (speculative)
        if (prevBranchPC[tid] != 0) {
            markovSuccessors[prevBranchPC[tid]][instPC]++;  // Increment frequency
        }
        prevBranchPC[tid] = instPC;
    }

    // -------------------------------------------------------------------------
    // Policy 5/6/7/8/9/10: Markov-based prefetching on L2 hit
    // -------------------------------------------------------------------------
    if (l1PrefetchPolicy == 5) {
        prefetchMarkovSuccessor(tid, instPC, true, 1, false, l2Latency,
                                type);   // Prefetch 1 to L1
    } else if (l1PrefetchPolicy == 6 || l1PrefetchPolicy == 7 || l1PrefetchPolicy == 8 ||
               finalMarkov) {
        unsigned numSucc = (finalMarkov && prefetchAllMarkovSuccessors) ? 100 : 1;
        if (limitRet && finalMarkov && prefetchAllMarkovSuccessors && type == BranchType::Return) {
            numSucc = 2;
        }
        prefetchMarkovSuccessor(tid, instPC, false, numSucc, false, l2Latency,
                                type, prefetchDepth);  // Prefetch to pBuffer
    } else if (l1PrefetchPolicy == 9) {
        prefetchMarkovSuccessor(tid, instPC, false, 2, false, l2Latency,
                                type);  // Prefetch 2 to pBuffer
    } else if (l1PrefetchPolicy == 10) {
        prefetchMarkovSuccessor(tid, instPC, false, 3, false, l2Latency,
                                type);  // Prefetch 3 to pBuffer
    }

    // -------------------------------------------------------------------------
    // Spatial locality stats (commented out for now)
    // -------------------------------------------------------------------------
    // auto& history = l1MissL2HitHistory[tid];
    // Addr targetAddr = l2_entry->target->instAddr();

    // if (!history.empty()) {
    //     // 1-history
    //     const auto& last = history.back();
    //     int64_t d1 = (int64_t)instPC - (int64_t)last.pc;
    //     int64_t d2 = (int64_t)instPC - (int64_t)last.target;
    //     multilevelstats.dist1HistoryPC.sample(d1);
    //     multilevelstats.dist1HistoryTarget.sample(d2);

    //     if (history.size() >= 2) {
    //         // 2-history
    //         const auto& secondLast = history[history.size() - 2];
    //         int64_t d3 = (int64_t)instPC - (int64_t)secondLast.pc;
    //         int64_t d4 = (int64_t)instPC - (int64_t)secondLast.target;
    //         multilevelstats.dist2HistoryPC.sample(d3);
    //         multilevelstats.dist2HistoryTarget.sample(d4);
    //     }
    // }

    // history.push_back({instPC, targetAddr});
    // if (history.size() > 2) {
    //     history.pop_front();
    // }

    if (bbMap_) {
        Addr targetAddr = l2_snapshot.target->instAddr();
        Addr fallThrough = instPC + minInstSize;
        auto baseLatency = l2Latency;

        bool doPfTarget = l2_snapshot.getPrefetchTarget();
        bool doPfThrough = l2_snapshot.getPrefetchThrough();

        bool isBackward = (l2_snapshot.target->instAddr() < instPC);
        applyNewPBitsLogic(type, taken, isBackward, doPfTarget, doPfThrough, L2Hit);
        int effectiveDepth = (depthOnlyCall && !isCall(type)) ? 1 : prefetchDepth;
        prevBwBranch.is_bw = isBackward;
        prevBwBranch.is_l2_miss = true;

        if (doPfTarget || doPfThrough) {
            uint64_t currentChainId = nextChainId++;
            
            if (doPfTarget) {
                prefetchViaBBMap(tid, targetAddr, true, false, effectiveDepth,
                    baseLatency, type, currentChainId, true);
                baseLatency = baseLatency + Cycles(1);
            }
            if (doPfThrough) {
                prefetchViaBBMap(tid, fallThrough, false, false, effectiveDepth,
                    baseLatency, type, currentChainId, true);
            }
        }
    }



    DPRINTF(BTB, "L2 BTB hit for PC %#x, latency=%d cycles, insert in L1\n",
            instPC, l2Latency);

    return BTBLookupResult(l1_victim->target.get(), l2Latency,
                           false, false, true, false, false, false, l1_victim->getDir());
}


BTBLookupResult
MultiLevelBTB::handleL3Hit(ThreadID tid, Addr instPC, BTBEntry *l3_entry,
                           BranchType type, bool taken)
{
    multilevelstats.l3Hits++;

    // Keep hierarchy non-redundant: on L3 hit, migrate the entry to L1..
    BTBEntry l3_snapshot(*l3_entry);
    DPRINTF(BTB, "%s(pc=%#x) -> L3[pc=%#x tgt=%#x], migrate to L1\n", __func__, instPC,
            l3_snapshot.getBranchAddr(), l3_snapshot.target->instAddr());
    l3btb.invalidate(l3_entry);

    BTBEntry *l1_entry = freeUpL1Entry(tid, instPC);
    assert(instPC == l3_snapshot.getBranchAddr()); // Ensure the write back has not modified the L3 entry.

    l1_entry->update(l3_snapshot);
    l1CompressedTagSync(instPC, tid, TagAction::Insert);
    DPRINTF(BTB, "L3 BTB hit for PC %#x, latency=%d cycles, insert in L1\n",
            instPC, l3Latency);

    return BTBLookupResult(l1_entry->target.get(), l3Latency,
                           false, false, false, true, false, false, l1_entry->getDir());
}

void
MultiLevelBTB::performShadowLookup(ThreadID tid, Addr instPC, BranchType type)
{
    bool realL1Hit = (l1btb.findEntry({instPC, tid}) != nullptr);
    bool realPBufferHit = (pBuffer.findEntry({instPC, tid}) != nullptr);
    bool realHit = realL1Hit || realPBufferHit;

    bool shadowL1Hit = false;
    int8_t hitPredType = -1;  // predecessor type of the hit entry

    BTBEntry *sL1_entry = shadowL1BTB.accessEntry({instPC, tid});
    if (sL1_entry != nullptr) {
        shadowL1Hit = true;
        hitPredType = sL1_entry->getMarkovPredType();
    } else {
        BTBEntry *sPB_entry = shadowPBuffer.accessEntry({instPC, tid});
        if (sPB_entry != nullptr) {
            shadowL1Hit = true;
            hitPredType = sPB_entry->getMarkovPredType();

            BTBEntry *sL1_victim = shadowL1BTB.findVictim({instPC, tid});
            shadowL1BTB.insertEntry({instPC, tid}, sL1_victim);
            sL1_victim->update(*sPB_entry->target, sPB_entry->inst);
            sL1_victim->setFromPBuffer(true);
            sL1_victim->setMarkovPredType(hitPredType);  // preserve predecessor type

            shadowPBuffer.invalidate(sPB_entry);

            prefetchShadowMarkovSuccessor(tid, instPC, 1, type);
        }
    }

    if (realHit && shadowL1Hit) {
        multilevelstats.shadowOverlaps[type]++;
    } else if (realHit && !shadowL1Hit) {
        multilevelstats.spatialOnlyHits[type]++;
    } else if (!realHit && shadowL1Hit) {
        multilevelstats.markovOnlyHits[type]++;
        // Record predecessor type breakdown
        // hitPredType == -1 means demand fill (not prefetched), map to index 0 (NoBranch)
        int idx = (hitPredType >= 0) ? hitPredType : 0;
        multilevelstats.markovOnlyHitsByPredType[idx]++;
    } else {
        BTBEntry *l2_check = l2btb.findEntry({instPC, tid});
        if (l2_check != nullptr) {
            multilevelstats.uniMisses[type]++;
        }
    }

    // ---- Shadow fill: whenever shadow misses, demand-fill from L2 ----
    if (!shadowL1Hit) {
        BTBEntry *l2_entry = l2btb.findEntry({instPC, tid});
        if (l2_entry != nullptr) {
            handleShadowL2Hit(tid, instPC, l2_entry, type);
        }
    }
}

void
MultiLevelBTB::handleShadowL2Hit(ThreadID tid, Addr instPC, BTBEntry* l2_entry, BranchType type)
{
    if (shadowPrevBranchPC[tid] != 0) {
        markovSuccessors[shadowPrevBranchPC[tid]][instPC]++;
    }
    shadowPrevBranchPC[tid] = instPC;

    BTBEntry *sL1_victim = shadowL1BTB.findVictim({instPC, tid});
    shadowL1BTB.insertEntry({instPC, tid}, sL1_victim);
    sL1_victim->update(*l2_entry->target, l2_entry->inst);
    sL1_victim->setMarkovPredType(-1);  // demand fill, no predecessor

    prefetchShadowMarkovSuccessor(tid, instPC, 1, type);
}

void
MultiLevelBTB::prefetchShadowMarkovSuccessor(ThreadID tid, Addr pc, unsigned numSuccessors, BranchType predType)
{
    auto it = markovSuccessors.find(pc);
    if (it == markovSuccessors.end() || it->second.empty()) {
        return;
    }

    std::vector<std::pair<Addr, uint64_t>> successors;
    for (const auto& [succ, freq] : it->second) {
        successors.push_back({succ, freq});
    }
    std::sort(successors.begin(), successors.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    unsigned prefetched = 0;
    for (const auto& [successor, freq] : successors) {
        if (prefetched >= numSuccessors) break;
        if (successor == 0) continue;

        if (shadowL1BTB.findEntry({successor, tid})) continue;

        BTBEntry *l2_entry = l2btb.findEntry({successor, tid});
        if (!l2_entry) continue;

        if (shadowPBuffer.findEntry({successor, tid})) continue;

        BTBEntry *sPB_victim = shadowPBuffer.findVictim({successor, tid});
        shadowPBuffer.insertEntry({successor, tid}, sPB_victim);
        sPB_victim->update(*l2_entry->target, l2_entry->inst);
        sPB_victim->setPrefetched(true);
        sPB_victim->setTimestamp(curCycle());
        sPB_victim->setMarkovPredType(static_cast<int8_t>(predType));
        multilevelstats.shadowPrefetches++;
    }
}

void
MultiLevelBTB::prefetchMarkovSuccessor(ThreadID tid, Addr pc, bool toL1,
                                       unsigned numSuccessors, bool triggeredByPBHit,
                                       Cycles baseLatency, BranchType triggerType, int depth)
{
    auto it = markovSuccessors.find(pc);
    if (it == markovSuccessors.end() || it->second.empty()) {
        return;  // No successor data for this PC
    }

    multilevelstats.mkHits++;

    // Build a vector of (successor, frequency) pairs and sort by frequency
    std::vector<std::pair<Addr, uint64_t>> successors;
    for (const auto& [succ, freq] : it->second) {
        successors.push_back({succ, freq});
    }

    // Sort by frequency (descending)
    std::sort(successors.begin(), successors.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    Cycles arrival = curCycle() + l2Latency + baseLatency;

    // Prefetch top N successors
    unsigned prefetched = 0;
    for (const auto& [successor, freq] : successors) {
        if (prefetched >= numSuccessors) {
            break;
        }

        if (successor == 0) {
            continue;
        }

        // Check if already in L1
        if (l1ApproxContains(successor, tid)) {
            continue;
        }
        // Find in L2
        BTBEntry *l2_entry = l2btb.findEntry({successor, tid});
        if (l2_entry) {
            multilevelstats.pfL2LookupHit++;
        } else {
            multilevelstats.pfL2LookupMiss++;
        }
        DPRINTF(BTB, "%s(spc=%#x) bpc=%#x, hit=%i\n", __func__, pc, successor, l2_entry!=nullptr);

        if (!l2_entry) {
            continue;
        }

        // Skip if already in pBuffer (for pBuffer-targeting prefetches)
        if (!toL1 && pbApproxContains(successor, tid)) {
            continue;
        }

        // multilevelstats.markovDist.sample(successor - pc);

        enqueuePrefetch(successor, tid, l2_entry, arrival, toL1,
                        triggeredByPBHit, 0, false, triggerType);

        if (depth > 2){
            BranchType nextTriggerType = getBranchType(l2_entry->inst);
            prefetchMarkovSuccessor(tid, successor, toL1, numSuccessors, triggeredByPBHit, arrival, nextTriggerType, depth - 1);
        }

        prefetched++;
    }
}

// trainMarkovOnCommit: Train Markov predictor on committed branches (Policy 7/8/9/10/finalMarkov)
void
MultiLevelBTB::trainMarkovOnCommit(ThreadID tid, Addr pc, Addr startAddr,
                                   Addr targetAddr, unsigned instSize,
                                   bool actuallyTaken, bool wasL2Hit)
{
    if (!(finalMarkov || l1PrefetchPolicy == 7 || l1PrefetchPolicy == 8 || l1PrefetchPolicy == 9 || l1PrefetchPolicy == 10)) {
        return;
    }
    if (finalMarkov) {
        // auto &prev = prevCommitBlockInfo[tid];
        // if (prev.valid) {
        //     //If current commit block's start address matches prev's target or fall-through
        //     if (startAddr == prev.target || startAddr == prev.fallThrough) {
        //         if (markovUseRecency) {
        //             markovSuccessors[prev.branchPC][pc] = curTick();
        //         } else {
        //             markovSuccessors[prev.branchPC][pc]++;
        //         }
        //     }
        // }
        // // Always update prev info with current branch
        // prev.branchPC = pc;
        // prev.target = targetAddr;
        // prev.fallThrough = pc + instSize;
        // prev.valid = true;
        // return;


        // auto &prev = prevCommitBlockInfo[tid];
        // if (prev.valid) {
        //     //If current commit block's start address matches prev's target or fall-through
        //     if (startAddr == prev.target || startAddr == prev.fallThrough) {
        //         if (markovUseRecency) {
        //             markovSuccessors[prev.branchPC][pc] = curTick();
        //         } else {
        //             markovSuccessors[prev.branchPC][pc]++;
        //         }
        //     }
        // }
        // // Always update prev info with current branch
        // prev.branchPC = pc;
        // prev.target = targetAddr;
        // prev.fallThrough = pc + instSize;
        // prev.valid = true;

        auto next_pc = actuallyTaken ? targetAddr : pc + instSize;

        auto it = bbMap_->find(next_pc);
        if (it == bbMap_->end())
            return;

        markovSuccessors[pc][it->second]++;
        return;
    }

    if (l1PrefetchPolicy != 7 && l1PrefetchPolicy != 8 &&
        l1PrefetchPolicy != 9 && l1PrefetchPolicy != 10) {
        return;
    }

    if(l1PrefetchPolicy == 7 && !wasL2Hit) {
        return;
    }

    if (prevBranchPC[tid] != 0) {
        if (markovUseRecency) {
            markovSuccessors[prevBranchPC[tid]][pc] = curTick();
        } else {
            markovSuccessors[prevBranchPC[tid]][pc]++;
        }
    }
    prevBranchPC[tid] = pc;
}

// trainPrefetchBitsOnCommit: Train prefetch bits at commit time
void
MultiLevelBTB::trainPrefetchBitsOnCommit(ThreadID tid, Addr pc, bool actuallyTaken, BranchType type)
{
    if (!trainBitsOnCommit)
        return;

    // Try L1 first
    BTBEntry *l1_entry = l1btb.findEntry({pc, tid});
    if (l1_entry) {
        if (!cleanBitsOnL1Promotion) {
            // Accumulative: only set bits to 1, never clear
            if (actuallyTaken) {
                l1_entry->setPrefetchTarget(true);
            } else {
                l1_entry->setPrefetchThrough(true);
            }
        } else {
            // Exclusive: set one, clear the other
            if (actuallyTaken) {
                l1_entry->setPrefetchTarget(true);
                l1_entry->setPrefetchThrough(false);
            } else {
                l1_entry->setPrefetchThrough(true);
                l1_entry->setPrefetchTarget(false);
            }
        }
        return;
    }
    if (updateDirOnlyL1)
        return;

    // L1 miss — try L2
    BTBEntry *l2_entry = l2btb.findEntry({pc, tid});
    if (l2_entry) {
        multilevelstats.trainBitsL1Miss++;
        if (!cleanBitsOnL1Promotion) {
            if (actuallyTaken) {
                l2_entry->setPrefetchTarget(true);
            } else {
                l2_entry->setPrefetchThrough(true);
            }
        } else {
            if (actuallyTaken) {
                l2_entry->setPrefetchTarget(true);
                l2_entry->setPrefetchThrough(false);
            } else {
                l2_entry->setPrefetchThrough(true);
                l2_entry->setPrefetchTarget(false);
            }
        }
        return;
    }

    if (enableL3) {
        BTBEntry *l3_entry = l3btb.findEntry({pc, tid});
        if (l3_entry) {
            multilevelstats.trainBitsL1Miss++;
            if (!cleanBitsOnL1Promotion) {
                if (actuallyTaken) {
                    l3_entry->setPrefetchTarget(true);
                } else {
                    l3_entry->setPrefetchThrough(true);
                }
            } else {
                if (actuallyTaken) {
                    l3_entry->setPrefetchTarget(true);
                    l3_entry->setPrefetchThrough(false);
                } else {
                    l3_entry->setPrefetchThrough(true);
                    l3_entry->setPrefetchTarget(false);
                }
            }
        }
    }
}

bool
MultiLevelBTB::usesPrefetchBitPolicy() const
{
    return trainBitsOnLookup || trainBitsOnCommit;
}

void
MultiLevelBTB::applyNewPBitsLogic(BranchType type, bool taken,
                bool isBackward,
                bool &doPfTarget, bool &doPfThrough,
                TriggerLocation triggerLoc)
{
    if ((triggerLoc == L1Hit && !prefetchOnL1Hit && !prefetchFwExitOnL1Hit)
      ||(triggerLoc == L2Hit && !usesPrefetchBitPolicy())
      ||(triggerLoc == PBHit && !prefetchOnPrefetchHit)
       ) {
        doPfTarget = false;
        doPfThrough = false;
        return;
    }
    if (newPBits) {

        bool isForwardExit = prevBwBranch.is_bw         // Was previous branch a backward branch
                          && doPfTarget && doPfThrough  // Was alternating
                          && !taken                     // We didn't exit the loop
                          && prevBwBranch.is_l2_miss    // If the backward branch was an L2 miss
                          ;

        if (forwardLoopExit && isForwardExit) {
            doPfTarget = true;
            doPfThrough = false;
            multilevelstats.pfTriggerFwExit++;
            return;
        }

        // Forward exits might have happen on L1 hits
        if (triggerLoc == L1Hit && !prefetchOnL1Hit) {
            doPfTarget = false;
            doPfThrough = false;
            return;
        }


        bool isCall = (type == BranchType::CallDirect || type == BranchType::CallIndirect);

        if (onlyCall && !isCall) {
            doPfTarget = false;
            doPfThrough = false;
            return;
        }

        if (onlyCallAndBackward && !isCall) {
            if (!isBackward || !doPfTarget || !doPfThrough) {
                doPfTarget = false;
                doPfThrough = false;
                return;
            }
        }

        if (callFallthrough && isCall) {
            doPfTarget = false;
            doPfThrough = true;
            multilevelstats.pfTriggerCall++;
            return;
        }

        if (backwardLoopExit && isBackward && taken && doPfTarget && doPfThrough) {
            doPfTarget = false;
            doPfThrough = true;
            multilevelstats.pfTriggerBwExit++;
            return;
        }

        if (!allConditional) {
            doPfTarget = false;
            doPfThrough = false;
            return;
        }

        if (doPfTarget && doPfThrough) {
            if (taken) {
                doPfTarget = false;
                doPfThrough = true;
            } else {
                doPfTarget = true;
                doPfThrough = false;
            }
            multilevelstats.pfTriggerCondAlt++;
        } else {
            doPfTarget = false;
            doPfThrough = false;
        }

        // if (isCall) {
        //     doPfTarget = false;
        //     doPfThrough = true;
        // }
    }
}

void
MultiLevelBTB::writebackToL2(ThreadID tid, BTBEntry *victim)
{
    assert(false);
    // There is no L1 entry evicted.
    if (!victim->target)
        return;
    Addr victimPC = victim->getBranchAddr();
    // Writeback trained status of the L1 victim to L2
    BTBEntry *l2_victim = l2btb.findVictim({victimPC, tid});
    l2btb.insertEntry({victimPC, tid}, l2_victim);
    l2_victim->update(*victim->target, victim->inst);
    l2_victim->copyDir(*victim);
    if (usesPrefetchBitPolicy()) {
        l2_victim->setPrefetchThrough(victim->getPrefetchThrough());
        l2_victim->setPrefetchTarget(victim->getPrefetchTarget());
    }
    multilevelstats.updatesL2miss++;
}


BTBEntry*
MultiLevelBTB::freeUpL1Entry(ThreadID tid, Addr instPC)
{
    assert(l1btb.findEntry({instPC, tid}) == nullptr);

    // Get L1 victim
    BTBEntry *l1_victim = l1btb.findVictim({instPC, tid}, false);
    if (l1_victim->isValid()) {

        // Perform writeback
        DPRINTF(BTB, "Evict L1[pc=%#x %s]\n", l1_victim->getBranchAddr(), l1_victim->print());

        // Create a new entry in the L2
        BTBEntry *l2_victim = l2btb.findEntry({l1_victim->getBranchAddr(), tid});
        if (l2_victim) {
            l2btb.accessEntry(l2_victim);
            DPRINTF(BTB, "Exists already in L2[pc=%#x %s]\n", l2_victim->getBranchAddr(), l2_victim->print());
            multilevelstats.updatesL2hits++;
        } else {
            l2_victim = l2btb.findVictim({l1_victim->getBranchAddr(), tid}, false);
            if (l2_victim->isValid()) {
                DPRINTF(BTB, "Evict L2[pc=%#x %s]\n", l2_victim->getBranchAddr(), l2_victim->print());
                if (enableL3) {
                    BTBEntry *l3_victim = l3btb.findEntry({l2_victim->getBranchAddr(), tid});
                    if (l3_victim) {
                        l3btb.accessEntry(l3_victim);
                        DPRINTF(BTB, "Exists already in L3[pc=%#x %s]\n", l3_victim->getBranchAddr(), l3_victim->print());
                        multilevelstats.updatesL3hits++;
                    } else {
                        l3_victim = l3btb.findVictim({l2_victim->getBranchAddr(), tid});
                        if (l3_victim) {
                            DPRINTF(BTB, "Evict L3[pc=%#x %s]\n", l3_victim->getBranchAddr(), l3_victim->print());
                        }
                        l3btb.insertEntry({l2_victim->getBranchAddr(), tid}, l3_victim);
                        multilevelstats.updatesL3miss++;
                    }
                    l3_victim->update(*l2_victim);
                    DPRINTF(BTB, "Updated L3[pc=%#x, tgt=%#x] %s\n",
                            l3_victim->getBranchAddr(), l3_victim->target->instAddr(), l3_victim->print());
                }
            }
            l2_victim->invalidate();
            l2btb.insertEntry({l1_victim->getBranchAddr(), tid}, l2_victim);
            multilevelstats.updatesL2miss++;
        }

        // Copy content from L1 victim to L2
        l2_victim->update(*l1_victim);
        DPRINTF(BTB, "Updated L2[pc=%#x, tgt=%#x] %s\n",
                l2_victim->getBranchAddr(), l2_victim->target->instAddr(), l2_victim->print());
    }
    l1_victim->invalidate();
    l1btb.insertEntry({instPC, tid}, l1_victim);
    return l1_victim;
}

void
MultiLevelBTB::recordPrevBlockInfo(ThreadID tid, Addr instPC, Addr targetAddr)
{
    auto &prev = prevBlockInfo[tid];
    prev.branchPC = instPC;
    prev.target = targetAddr;
    prev.fallThrough = instPC + minInstSize;
    prev.valid = true;
}

bool
MultiLevelBTB::isChainDead(uint64_t chainId) const
{
    for (const auto& entry : deferredPrefetchQueue) {
        if (entry.chainId == chainId)
            return false;
    }
    return true;
}

void
MultiLevelBTB::recordChainEnd(ActiveChainEntry &chain)
{
    switch (chain.lastEndReason) {
        case ChainEndReason::Evicted:
            multilevelstats.chainEndEvicted++;
            break;
        case ChainEndReason::Demand:
            multilevelstats.chainEndDemand++;
            break;
        case ChainEndReason::Presence:
            multilevelstats.chainEndPresence++;
            break;
        case ChainEndReason::L2Miss:
            multilevelstats.chainEndL2Miss++;
            break;
        case ChainEndReason::RetFilter:
            multilevelstats.chainEndRetFilter++;
            break;
        case ChainEndReason::DepthExhaust:
            multilevelstats.chainEndDepthExhaust++;
            break;
        case ChainEndReason::None:
            multilevelstats.chainEndNonEntry++;
            break;
    }
    // Reset after recording
    chain.lastEndReason = ChainEndReason::None;
}

void
MultiLevelBTB::prefetchViaBBMap(ThreadID tid, Addr lookupAddr,
                                bool isTakenPath, bool triggeredByPBHit,
                                int depth, Cycles baseLatency,
                                BranchType triggerType, uint64_t chainId,
                                bool allocateChain)
{
    auto it = bbMap_->find(lookupAddr);
    if (it == bbMap_->end())
        return;

    Addr pfPC = it->second;

    BTBEntry *l2_pf = l2btb.findEntry({pfPC, tid});
    // Kill the trigger if L2-miss
    if (!l2_pf && allocateChain)
        return;
    if (limitRet && triggerType == BranchType::Return) {
        return;
    }

    baseLatency = baseLatency + Cycles(1);
    Cycles issueTime = curCycle() + baseLatency;

    // Insert into deferred queue instead of directly into pBuffer
    DeferredPrefetchEntry dfEntry;
    dfEntry.pc = pfPC;
    dfEntry.tid = tid;
    dfEntry.issueTime = issueTime;
    dfEntry.toL1 = false;
    dfEntry.triggeredByPBHit = triggeredByPBHit;
    dfEntry.takenPrefetched = isTakenPath;
    dfEntry.triggerType = triggerType;
    dfEntry.chainId = chainId;
    dfEntry.allocateChain = allocateChain;
    dfEntry.depth = depth;

    // Sorted insertion by issueTime (ascending)
    auto pos = std::lower_bound(deferredPrefetchQueue.begin(),
                                deferredPrefetchQueue.end(), dfEntry,
                                [](const DeferredPrefetchEntry& a,
                                   const DeferredPrefetchEntry& b) {
                                    return a.issueTime < b.issueTime;
                                });
    deferredPrefetchQueue.insert(pos, dfEntry);
}

void
MultiLevelBTB::processDeferredPrefetchQueue()
{
    schedule(pfqEvent, clockEdge(Cycles(1)));
    if (!deferredPrefetchQueue.empty()) {
        std::vector<uint64_t> activeChains;
        for (const auto& entry : deferredPrefetchQueue) {
            if (entry.chainId != 0 && std::find(activeChains.begin(), activeChains.end(), entry.chainId) == activeChains.end()) {
                if (maxChainTrackerEntries > 0) {
                    int tableIdx = entry.chainId % maxChainTrackerEntries;
                    if (chainTable[tableIdx].chainId != entry.chainId)
                        continue;  
                }
                activeChains.push_back(entry.chainId);
            }
        }
        multilevelstats.parallelChains.sample(activeChains.size());
    }

    auto readyEnd = deferredPrefetchQueue.begin();
    while (readyEnd != deferredPrefetchQueue.end()) {
        if (!noPrefetchLatency && readyEnd->issueTime > curCycle())
            break;
        ++readyEnd;
    }

    std::vector<DeferredPrefetchEntry> readyEntries(deferredPrefetchQueue.begin(), readyEnd);
    deferredPrefetchQueue.erase(deferredPrefetchQueue.begin(), readyEnd);

    for (auto& entry : readyEntries) {
        Addr pc = entry.pc;
        bool validChain = true;
        ActiveChainEntry* chainEntry = nullptr;

        if (entry.chainId != 0 && maxChainTrackerEntries > 0) {
            int tableIdx = entry.chainId % maxChainTrackerEntries;
            chainEntry = &chainTable[tableIdx];
            if (chainEntry->chainId != entry.chainId) {
                if (!entry.allocateChain) {
                    validChain = false;
                }
            } else if (chainEntry->remainingPrefetches == 0) {
                validChain = false;
            }
        }

        if (!validChain) {
            continue;
        }

        multilevelstats.enqueueDeferred++;

        // Skip prefetch if the same PC is being demand-accessed this cycle.
        if (isDemandAccess(pc)) {
            multilevelstats.skipDuetoDemand++;
            if (!entry.allocateChain) {
                chainEntry->lastEndReason = ChainEndReason::Demand;
            }
            continue;
        }

        bool hit = l1ApproxContains(pc, entry.tid) || pbApproxContains(pc, entry.tid);
        if (hit && chainEntry && !entry.allocateChain) {
            chainEntry->lastEndReason = ChainEndReason::Presence;
        }

        if (!hit) {
            BTBEntry *l2_pf = l2btb.findEntry({pc, entry.tid});
            if (l2_pf) {
                Cycles arrival = entry.issueTime + l2Latency;
                enqueuePrefetch(pc, entry.tid, l2_pf, arrival,
                               entry.toL1, entry.triggeredByPBHit,
                               0, entry.takenPrefetched,
                               entry.triggerType);
                multilevelstats.pfL2LookupHit++;

                if (entry.allocateChain && maxChainTrackerEntries > 0) {
                    int tableIdx = entry.chainId % maxChainTrackerEntries;
                    if (chainTable[tableIdx].chainId != entry.chainId) {
                        // Old chain is being evicted — record stats
                        auto& oldChain = chainTable[tableIdx];
                        if (oldChain.chainId != 0) {
                            bool dead = isChainDead(oldChain.chainId);
                            if (dead || oldChain.remainingPrefetches == 0) {
                                // Chain is dead and has a specific end reason
                                recordChainEnd(oldChain);
                            } else {
                                // Chain still has in-flight entries but slot is stolen
                                multilevelstats.chainEndEvicted++;
                            }
                            auto issuedPF = entry.depth - oldChain.remainingPrefetches;
                            multilevelstats.prefetchesPerTrigger.sample(issuedPF);
                        }
                        
                        chainTable[tableIdx].chainId = entry.chainId;
                        chainTable[tableIdx].remainingPrefetches = entry.depth;
                        chainTable[tableIdx].lastEndReason = ChainEndReason::None;
                    }
                    chainEntry = &chainTable[tableIdx];
                }

                if (chainEntry && chainEntry->remainingPrefetches > 0) {
                    chainEntry->remainingPrefetches--;
                    bool completedChain = false;
                    if (chainEntry->remainingPrefetches == 0) {
                        chainEntry->lastEndReason = ChainEndReason::DepthExhaust;
                        completedChain = true;
                    }
                    Addr targetAddr = l2_pf->target->instAddr();
                    Addr fallThrough = pc + minInstSize;
                    Cycles nextBaseLatency = (arrival > curCycle()) ? (arrival - curCycle()) : Cycles(0);
                    BranchType l2PfType = getBranchType(l2_pf->inst);

                    if (l2_pf->getPrefetchTarget() && !completedChain) {
                        prefetchViaBBMap(entry.tid, targetAddr, true, entry.triggeredByPBHit,
                                     1, nextBaseLatency, l2PfType, entry.chainId);
                        nextBaseLatency += Cycles(1);
                        if (limitRet && l2PfType == BranchType::Return) {
                            chainEntry->lastEndReason = ChainEndReason::RetFilter;
                        }
                    }
                    if (l2_pf->getPrefetchThrough() && !completedChain) {
                        prefetchViaBBMap(entry.tid, fallThrough, false, entry.triggeredByPBHit,
                                     1, nextBaseLatency, l2PfType, entry.chainId);
                    }
                }
            } else {
                multilevelstats.pfL2LookupMiss++;
                if (chainEntry && !entry.allocateChain) {
                    chainEntry->lastEndReason = ChainEndReason::L2Miss;
                }
            }
        } else {
            multilevelstats.skipDuetoPresence++;
        }
    }

    currentCycleDemand.clear();
}

bool
MultiLevelBTB::l1ApproxContains(Addr pc, ThreadID tid)
{
    // If filter disabled, fall back to exact L1 lookup
    if (!useCompressedTagFilter)
        return l1btb.findEntry({pc, tid}) != nullptr;

    bool approxHit = l1CompressedTags.findEntry({pc, tid}) != nullptr;
    bool exactHit = l1btb.findEntry({pc, tid}) != nullptr;

    multilevelstats.compressedTagChecks++;
    if (approxHit && !exactHit) {
        multilevelstats.compressedTagFalsePositives++;
    } else if (!approxHit && exactHit) {
        multilevelstats.compressedTagFalseNegatives++;
    }

    auto comp_entry = l1CompressedTags.findEntry({pc, tid});
    auto full_entry = l1btb.findEntry({pc, tid});
    if (comp_entry && full_entry && comp_entry->getBranchAddr() != full_entry->getBranchAddr()) {
        multilevelstats.compressedTagAliases++;
    }

    return approxHit;
}

bool
MultiLevelBTB::pbApproxContains(Addr pc, ThreadID tid)
{
    // If filter disabled, fall back to exact L1 lookup
    if (!useCompressedTagFilter)
        return pBuffer.findEntry({pc, tid}) != nullptr;

    bool approxHit = pbCompressedTags.findEntry({pc, tid}) != nullptr;
    bool exactHit = pBuffer.findEntry({pc, tid}) != nullptr;

    multilevelstats.compressedTagChecks++;
    if (approxHit && !exactHit) {
        multilevelstats.compressedTagFalsePositives++;
    } else if (!approxHit && exactHit) {
        multilevelstats.compressedTagFalseNegatives++;
    }

    auto comp_entry = pbCompressedTags.findEntry({pc, tid});
    auto full_entry = pBuffer.findEntry({pc, tid});
    if (comp_entry && full_entry && comp_entry->getBranchAddr() != full_entry->getBranchAddr()) {
        multilevelstats.compressedTagAliases++;
    }

    return approxHit;
}

void
MultiLevelBTB::l1CompressedTagSync(Addr pc, ThreadID tid, TagAction action)
{
    if (!useCompressedTagFilter)
        return;

    if (action == TagAction::Hit) {
        l1CompressedTags.accessEntry({pc, tid});
    } else if (action == TagAction::Insert) {
        BTBEntry *entry = l1CompressedTags.findEntry({pc, tid});
        if (entry) {
            l1CompressedTags.accessEntry(entry);
        } else {
            entry = l1CompressedTags.findVictim({pc, tid});
            l1CompressedTags.insertEntry({pc, tid}, entry);
        }
    }
}

void
MultiLevelBTB::pbCompressedTagSync(Addr pc, ThreadID tid, TagAction action)
{
    if (!useCompressedTagFilter)
        return;

    if (action == TagAction::Insert) {
        BTBEntry *entry = pbCompressedTags.findVictim({pc, tid});
        pbCompressedTags.insertEntry({pc, tid}, entry);
    } else if (action == TagAction::Invalidate) {
        BTBEntry *entry = pbCompressedTags.findEntry({pc, tid});
        if (entry) pbCompressedTags.invalidate(entry);
    }
}

} // namespace gem5::branch_prediction
