#include "cpu/pred/multilevel_btb.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/BTB.hh"

namespace gem5::branch_prediction
{
MultiLevelBTB::MultiLevelBTBStats::MultiLevelBTBStats(statistics::Group *parent, MultiLevelBTB *btb)
    : statistics::Group(parent),
      ADD_STAT(successorCountDist, statistics::units::Count::get(),
               "Distribution of successor counts for L1 miss L2 hit branches"),
      ADD_STAT(dist1HistoryPC, statistics::units::Count::get(), "Distance (PC - LastPC) for 1-history"),
      ADD_STAT(dist1HistoryTarget, statistics::units::Count::get(), "Distance (PC - LastTarget) for 1-history"),
      ADD_STAT(dist2HistoryPC, statistics::units::Count::get(), "Distance (PC - 2ndLastPC) for 2-history"),
      ADD_STAT(dist2HistoryTarget, statistics::units::Count::get(), "Distance (PC - 2ndLastTarget) for 2-history"),
      ADD_STAT(l1MissL2Hits, statistics::units::Count::get(), "Number of L1 misses that hit in L2"),
      ADD_STAT(uselessPrefetches, statistics::units::Count::get(), "Number of useless prefetches (L1 direct prefetch evicted)"),
      ADD_STAT(totalPrefetches, statistics::units::Count::get(), "Total number of prefetches"),
      // Unified prefetch coverage
      ADD_STAT(prefetchHits, statistics::units::Count::get(), "Useful prefetches (pBuffer hit + L1 reuse)"),
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
      btb(btb)
{
    using namespace statistics;
    successorCountDist.init(0).flags(total | pdf);
    dist1HistoryPC.init(0).flags(total | pdf);
    dist1HistoryTarget.init(0).flags(total | pdf);
    dist2HistoryPC.init(0).flags(total | pdf);
    dist2HistoryTarget.init(0).flags(total | pdf);

    l1MissL2Hits.flags(total);
    uselessPrefetches.flags(total);
    totalPrefetches.flags(total);
    prefetchDistCount.init(16);
    prefetchDistUsed.init(16);
    prefetchUsefulness = prefetchDistUsed / prefetchDistCount;
    prefetchUsefulness.precision(3);
    predMatches.init(15);
    predChecks.init(15);

    predMatchRatio = predMatches / predChecks;

    // Unified prefetch coverage formula
    prefetchCoverage = prefetchHits / (prefetchHits + totalPrefetches);
    prefetchCoverage.precision(3);
    
    // pBuffer useless rate
    pBufferUselessRate = uselessPrefetches / totalPrefetches;
    pBufferUselessRate.precision(3);
    
    // L1 useless prefetch rate (for pBuffer-installed entries)
    l1UselessInstalledRate = l1InstalledEvicted / l1Installed;
    l1UselessInstalledRate.precision(3);

    numBranchesPerPrefetch.init(0, 64, 1);
}

void
MultiLevelBTB::MultiLevelBTBStats::preDumpStats()
{
    statistics::Group::preDumpStats();
    
    for (const auto& pair : btb->l1MissL2HitSuccessors) {
        size_t count = pair.second.size();
        successorCountDist.sample(count);
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
      l1Latency(p.l1Latency),
      l2Latency(p.l2Latency),
      minInstSize(p.minInstSize),
      l1PrefetchPolicy(p.l1PrefetchPolicy),
      prefetchOnlyForward(p.prefetchOnlyForward),
      multilevelstats(this, this),
      l1MissL2HitHistory(p.numThreads),
      prevBranchPC(p.numThreads, 0)
{
    DPRINTF(BTB, "MultiLevelBTB: Creating L1(%d entries, %d cycles) + L2(%d entries, %d cycles)\n",
            p.l1NumEntries, p.l1Latency, p.l2NumEntries, p.l2Latency);

    if (!isPowerOf2(p.l1NumEntries) || !isPowerOf2(p.l2NumEntries)) {
        fatal("BTB entries must be power of 2!");
    }
}

void
MultiLevelBTB::memInvalidate()
{
    l1btb.clear();
    l2btb.clear();
    pBuffer.clear();
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
    return false;
}

const PCStateBase *
MultiLevelBTB::lookup(ThreadID tid, Addr instPC, BranchType type)
{
    BTBLookupResult result = lookupWithLatency(tid, instPC, type);
    return result.target;
}

BTBLookupResult
MultiLevelBTB::lookupWithLatency(ThreadID tid, Addr instPC, BranchType type, bool taken)
{
    stats.lookups[type]++;

    // lookup l1 btb firstly
    BTBEntry *l1_entry = l1btb.accessEntry({instPC, tid});
    Cycles extraLatency = Cycles(0);
    bool isPrefetchHit = false;
    uint8_t prefetchDistance = 0;
    if (l1_entry != nullptr) {
        if (l1_entry->isPrefetched()) {
            isPrefetchHit = true;
            multilevelstats.prefetchHits++;
            if (l1_entry->getPrefetchDistance() < 16) {
                prefetchDistance = l1_entry->getPrefetchDistance();
                multilevelstats.prefetchDistUsed[l1_entry->getPrefetchDistance()]++;
            }
            l1_entry->setPrefetched(false);
            
            Cycles delta = curCycle() - l1_entry->getTimestamp();
            if (delta < l2Latency) {
                extraLatency = l2Latency - delta;
            }

            if (l1PrefetchPolicy > 2 && l1PrefetchPolicy != 4) {
                Addr nextRegionStart = (instPC & ~127) + 128;
                Addr endOffset = 128;
                size_t numBranches = 0;
                for (Addr offset = minInstSize; offset <= endOffset; offset += minInstSize) {
                    Addr pfAddr = nextRegionStart + offset;
                    BTBEntry *l2_pf = l2btb.findEntry({pfAddr, tid});
                    if (l2_pf) {
                        BTBEntry *l1_pf_check = l1btb.findEntry({pfAddr, tid});
                        if (!l1_pf_check) {
                            BTBEntry *l1_pf_victim = l1btb.findVictim({pfAddr, tid});
                            if (l1_pf_victim->isPrefetched()) {
                                multilevelstats.uselessPrefetches++;
                                l1_pf_victim->setPrefetched(false);
                            }
                            l1btb.insertEntry({pfAddr, tid}, l1_pf_victim);
                            l1_pf_victim->update(*l2_pf->target, l2_pf->inst);
                            l1_pf_victim->setPrefetched(true);
                            l1_pf_victim->setTimestamp(curCycle());
                            if (numBranches < 16) {
                                multilevelstats.prefetchDistCount[numBranches]++;
                                l1_pf_victim->setPrefetchDistance(numBranches);
                            }
                            multilevelstats.totalPrefetches++;
                            numBranches++;
                        }
                    }
                }
                multilevelstats.numBranchesPerPrefetch.sample(numBranches);
            }
            
        } else if (l1_entry->isFromPBuffer()) {
            l1_entry->setFromPBuffer(false);  // Only count first reuse
        }
        
        DPRINTF(BTB, "L1 BTB hit for PC %#x, latency=%d cycles\n", instPC, l1Latency + extraLatency);
        
        bool predMatch = false;
        if (isPrefetchHit && cPred) {
            // Check if the stored prediction matches the current prediction (taken)
            multilevelstats.predChecks[prefetchDistance]++;
            if (l1_entry->getPredTaken() == taken) {
                if(prefetchDistance == 0) {
                    predMatch = true;
                }
                multilevelstats.predMatches[prefetchDistance]++;
            }
        }

        return BTBLookupResult(l1_entry->target.get(), l1Latency + extraLatency, true, false, isPrefetchHit, predMatch);
    }

    if (l1PrefetchPolicy == 4) {
        BTBEntry *pB_entry = pBuffer.accessEntry({instPC, tid});
        if (pB_entry != nullptr) {
            if (pB_entry->isPrefetched()) {
                multilevelstats.prefetchHits++;  // Count as useful prefetch
                pB_entry->setPrefetched(false);
            }
            if (pB_entry->getPrefetchDistance() < 16) {
                multilevelstats.prefetchDistUsed[pB_entry->getPrefetchDistance()]++;
            }
            
            BTBEntry *l1_victim = l1btb.findVictim({instPC, tid});
            // Track L1 victim eviction for pBuffer-installed entries
            if (l1_victim->isFromPBuffer()) {
                // L1 entry from pBuffer evicted without reuse
                multilevelstats.l1InstalledEvicted++;
            }
            l1btb.insertEntry({instPC, tid}, l1_victim);
            l1_victim->update(*pB_entry->target, pB_entry->inst);
            l1_victim->setFromPBuffer(true);  // Mark as installed from pBuffer for reuse tracking
            l1_victim->setPrefetchDistance(pB_entry->getPrefetchDistance());
            l1_victim->setPredTaken(pB_entry->getPredTaken());
            l1_victim->setPrefetched(false);

            multilevelstats.l1Installed++;  // Count entries installed from pBuffer to L1
            
            bool predMatch = false;
            if (cPred) {
                multilevelstats.predChecks[pB_entry->getPrefetchDistance()]++;
                if (pB_entry->getPredTaken() == taken) {
                    if(pB_entry->getPrefetchDistance() == 0) {
                        predMatch = true;
                    }
                    multilevelstats.predMatches[pB_entry->getPrefetchDistance()]++;
                }
            }
            DPRINTF(BTB, "pBuffer hit for PC %#x, promoted to L1\n", instPC);
            return BTBLookupResult(pB_entry->target.get(), l1Latency, true, false, true, predMatch);
        }
    }

    // L1miss, lookup l2 btb
    BTBEntry *l2_entry = l2btb.accessEntry({instPC, tid});
    if (l2_entry != nullptr) {
        multilevelstats.l1MissL2Hits++;
        
        auto l1_victim = l1btb.findVictim({instPC, tid});
        if (l1_victim->isPrefetched()) {
            multilevelstats.uselessPrefetches++;
            l1_victim->setPrefetched(false);
        } else if (l1_victim->isFromPBuffer()) {
            // L1 entry from pBuffer evicted without reuse
            multilevelstats.l1InstalledEvicted++;
            l1_victim->setFromPBuffer(false);
        }
        l1btb.insertEntry({instPC, tid}, l1_victim);
        l1_victim->update(*l2_entry->target, l2_entry->inst);
        

        // Prefetching from L2 to L1 BTB.
        bool doPrefetch = true;
        if (prefetchOnlyForward) {
            Addr targetAddr = l2_entry->target->instAddr();
            bool isBackward = (targetAddr < instPC);
            if (type == BranchType::DirectCond || type == BranchType::IndirectCond) {
                if (isBackward && taken) {
                    doPrefetch = false;
                }
            } else {
                doPrefetch = !isBackward;
            }
        }

        if (l1PrefetchPolicy > 0 && doPrefetch) {
            Addr endOffset = 128;
            if (l1PrefetchPolicy > 1 && l1PrefetchPolicy != 4) {
                Addr regionEnd = (instPC & ~127) + 128;
                endOffset = regionEnd - instPC + 128;
            }
            size_t numBranches = 0;
            for (Addr offset = minInstSize; offset <= endOffset; offset += minInstSize) {
                Addr pfAddr = instPC + offset;
                BTBEntry *l2_pf = l2btb.findEntry({pfAddr, tid});
                if (l2_pf) {
                    BTBEntry *l1_pf_check = l1btb.findEntry({pfAddr, tid});
                    if (!l1_pf_check) {
                        multilevelstats.totalPrefetches++;
                        numBranches++;
                        if (l1PrefetchPolicy == 4) {
                                 BTBEntry *pB_victim = pBuffer.findVictim({pfAddr, tid});
                                 // Check if pBuffer victim was a prefetched entry that was never used
                                 if (pB_victim->isPrefetched()) {
                                     multilevelstats.uselessPrefetches++;
                                 }
                                 pBuffer.insertEntry({pfAddr, tid}, pB_victim);
                                 pB_victim->update(*l2_pf->target, l2_pf->inst);
                                 pB_victim->setPrefetched(true);
                                 pB_victim->setTimestamp(curCycle());
                                 if (numBranches < 16) {
                                     multilevelstats.prefetchDistCount[numBranches]++;
                                     pB_victim->setPrefetchDistance(numBranches);
                                 }
                                //  if (cPred && l2_pf->inst && l2_pf->inst->isCondCtrl()) {
                                //      bool pred = cPred->predictNoUpdate(tid, pfAddr, true);
                                //      pB_victim->setPredTaken(pred);
                                //  }
                        } else {
                            BTBEntry *l1_pf_victim = l1btb.findVictim({pfAddr, tid});
                            if (l1_pf_victim->isPrefetched()) {
                                multilevelstats.uselessPrefetches++;
                                l1_pf_victim->setPrefetched(false);
                            }
                            l1btb.insertEntry({pfAddr, tid}, l1_pf_victim);
                            l1_pf_victim->update(*l2_pf->target, l2_pf->inst);
                            l1_pf_victim->setPrefetched(true);
                            l1_pf_victim->setTimestamp(curCycle());
                            if (numBranches < 16) {
                                    multilevelstats.prefetchDistCount[numBranches]++;
                                    l1_pf_victim->setPrefetchDistance(numBranches);
                            }
                            
                            // Use predictNoUpdate if available
                            // if (cPred && l2_pf->inst && l2_pf->inst->isCondCtrl()) {
                            //     bool pred = cPred->predictNoUpdate(tid, pfAddr, true);
                            //     l1_pf_victim->setPredTaken(pred);
                            // }
                        }
                    }
                }
            }
            multilevelstats.numBranchesPerPrefetch.sample(numBranches);
        }
        

        if (prevBranchPC[tid] != 0) {
            l1MissL2HitSuccessors[prevBranchPC[tid]].insert(instPC);
        }
        prevBranchPC[tid] = instPC;
        if (l1MissL2HitSuccessors.find(instPC) == l1MissL2HitSuccessors.end()) {
            l1MissL2HitSuccessors[instPC] = std::set<Addr>();
        }

        // Spatial locality stats
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

        DPRINTF(BTB, "L2 BTB hit for PC %#x, latency=%d cycles, insert in L1\n", instPC, l2Latency);
        return BTBLookupResult(l2_entry->target.get(), l2Latency, false, true);
    }
    // Miss in both l1 and l2
    stats.misses[type]++;
    DPRINTF(BTB, "BTB miss for PC %#x\n", instPC);
    return BTBLookupResult(nullptr, l1Latency + l2Latency, false, false);
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
    return nullptr;
}

void
MultiLevelBTB::update(ThreadID tid, Addr instPC,
                      const PCStateBase &target,
                      BranchType type, StaticInstPtr inst)
{
    stats.updates[type]++;

    
    BTBEntry *l2_victim = l2btb.findVictim({instPC, tid});
    l2btb.insertEntry({instPC, tid}, l2_victim);
    l2_victim->update(target, inst);

  
    BTBEntry *l1_victim = l1btb.findVictim({instPC, tid});
    l1btb.insertEntry({instPC, tid}, l1_victim);
    l1_victim->update(target, inst);

    DPRINTF(BTB, "Updated BTB for PC %#x -> %#x\n", instPC, target.instAddr());
}
} // namespace gem5::branch_prediction