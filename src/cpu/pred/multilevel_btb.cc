#include "cpu/pred/multilevel_btb.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/BTB.hh"
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
      ADD_STAT(shadowOverlaps, statistics::units::Count::get(), "Hit in BOTH Real and Shadow"),
      ADD_STAT(spatialOnlyHits, statistics::units::Count::get(), "Hit in Real, Miss in Shadow"),
      ADD_STAT(markovOnlyHits, statistics::units::Count::get(), "Miss in Real, Hit in Shadow"),
      ADD_STAT(uniMisses, statistics::units::Count::get(), "Miss in BOTH, but Hit in L2"),
      ADD_STAT(markovOnlyHitsByPredType, statistics::units::Count::get(), "Markov only hits broken down by predecessor type"),
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
    uselessPrefetches.flags(total);
    totalPrefetches.flags(total);
    prefetchDistCount.init(16);
    prefetchDistUsed.init(16);
    prefetchUsefulness = prefetchDistUsed / prefetchDistCount;
    prefetchUsefulness.precision(3);
    predMatches.init(15);
    predChecks.init(15);

    predMatchRatio = predMatches / predChecks;

    // Policy 11 Shadow Stats
    shadowOverlaps.init(enums::Num_BranchType).flags(total | pdf);
    spatialOnlyHits.init(enums::Num_BranchType).flags(total | pdf);
    markovOnlyHits.init(enums::Num_BranchType).flags(total | pdf);
    uniMisses.init(enums::Num_BranchType).flags(total | pdf);
    
    markovOnlyHitsByPredType.init(enums::Num_BranchType).flags(total | pdf);
    for (int i = 0; i < enums::Num_BranchType; i++) {
        shadowOverlaps.subname(i, enums::BranchTypeStrings[i]);
        spatialOnlyHits.subname(i, enums::BranchTypeStrings[i]);
        markovOnlyHits.subname(i, enums::BranchTypeStrings[i]);
        uniMisses.subname(i, enums::BranchTypeStrings[i]);
        markovOnlyHitsByPredType.subname(i, enums::BranchTypeStrings[i]);
    }
    // Unified prefetch coverage formula
    prefetchCoverage = prefetchHits / (prefetchHits + l1MissL2Hits);
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
    
    for (const auto& pair : btb->markovSuccessors) {
        size_t count = pair.second.size();
        successorCountDist.sample(count);
    }
    btb->markovSuccessors.clear();
    std::fill(btb->prevBranchPC.begin(), btb->prevBranchPC.end(), 0);
    std::fill(btb->shadowPrevBranchPC.begin(), btb->shadowPrevBranchPC.end(), 0);
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
      prevBranchPC(p.numThreads, 0),
      shadowPrevBranchPC(p.numThreads, 0),
      shadowL1BTB("shadowL1BTB", p.l1NumEntries, p.l1Associativity,
            p.l1ReplPolicy, p.l1IndexingPolicy, BTBEntry(genTagExtractor(p.l1IndexingPolicy))),
      shadowPBuffer("shadowPrefetchBuffer", p.pBufferSize, 1, p.pBufferReplPolicy, p.pBufferIndexingPolicy, BTBEntry(genTagExtractor(p.pBufferIndexingPolicy)))
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
    shadowL1BTB.clear();
    shadowPBuffer.clear();
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

    if (l1PrefetchPolicy == 11) {
        performShadowLookup(tid, instPC, type);
    }

    // ==========================================================================
    // Step 1: L1 BTB lookup
    // ==========================================================================
    BTBEntry *l1_entry = l1btb.accessEntry({instPC, tid});
    if (l1_entry != nullptr) {
        return handleL1Hit(tid, instPC, l1_entry, taken);
    }

    // ==========================================================================
    // Step 2: pBuffer lookup (Policy 4, 6, 7, 8, 9, and 10 - all use pBuffer)
    // ==========================================================================
    if (l1PrefetchPolicy == 4 || l1PrefetchPolicy == 6 || l1PrefetchPolicy == 7 || 
        l1PrefetchPolicy == 8 || l1PrefetchPolicy == 9 || l1PrefetchPolicy == 10 ||
        l1PrefetchPolicy == 11) {
        BTBEntry *pB_entry = pBuffer.accessEntry({instPC, tid});
        if (pB_entry != nullptr) {
            return handlePBufferHit(tid, instPC, pB_entry, taken);
        }
    }


    // ==========================================================================
    // Step 3: L2 BTB lookup
    // ==========================================================================
    BTBEntry *l2_entry = l2btb.accessEntry({instPC, tid});
    if (l2_entry != nullptr) {
        return handleL2Hit(tid, instPC, l2_entry, type, taken);
    }

    // Miss in both l1 and l2
    stats.misses[type]++;
    DPRINTF(BTB, "BTB miss for PC %#x\n", instPC);
    return BTBLookupResult(nullptr, l1Latency + l2Latency, false, false, false);
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

//=============================================================================
// handleL1Hit: Handle L1 BTB hit
//
// This function handles all L1 BTB hit scenarios:
// - Normal L1 hit (entry was not prefetched)
// - Prefetched entry hit: update stats, compute extra latency
// - Policy 3 (NextRegionOnL1Hit): trigger next-region prefetch on prefetch hit
// - Prediction match checking for prefetched entries
//=============================================================================
BTBLookupResult
MultiLevelBTB::handleL1Hit(ThreadID tid, Addr instPC, BTBEntry *l1_entry,
                           bool taken)
{
    Cycles extraLatency = Cycles(0);
    bool isPrefetchHit = false;
    uint8_t prefetchDistance = 0;

    // -------------------------------------------------------------------------
    // Case 1: Hit on a prefetched entry
    // -------------------------------------------------------------------------
    if (l1_entry->isPrefetched()) {
        isPrefetchHit = true;
        multilevelstats.prefetchHits++;

        // Track prefetch distance statistics (only for spatial prefetch policies 1-4)
        if ((l1PrefetchPolicy <= 4 || l1PrefetchPolicy == 11) && l1_entry->getPrefetchDistance() < 16) {
            prefetchDistance = l1_entry->getPrefetchDistance();
            multilevelstats.prefetchDistUsed[prefetchDistance]++;
        }
        l1_entry->setPrefetched(false);

        // Compute extra latency if prefetch hasn't fully completed
        Cycles delta = curCycle() - l1_entry->getTimestamp();
        if (delta < l2Latency) {
            extraLatency = l2Latency - delta;
        }

        // ---------------------------------------------------------------------
        // Policy 3 (NextRegionOnL1Hit): Prefetch next region on L1 prefetch hit
        // Condition: l1PrefetchPolicy > 2 && l1PrefetchPolicy != 4
        //            which means l1PrefetchPolicy == 3
        // ---------------------------------------------------------------------
        if (l1PrefetchPolicy == 3) {
            Addr nextRegionStart = (instPC & ~127) + 128;
            Addr endOffset = 128;
            size_t numBranches = 0;

            for (Addr offset = minInstSize; offset <= endOffset; offset += minInstSize) {
                Addr pfAddr = nextRegionStart + offset;
                BTBEntry *l2_pf = l2btb.findEntry({pfAddr, tid});
                if (l2_pf) {
                    BTBEntry *l1_pf_check = l1btb.findEntry({pfAddr, tid});
                    if (!l1_pf_check) {
                        // Evict victim and track useless prefetches
                        BTBEntry *l1_pf_victim = l1btb.findVictim({pfAddr, tid});
                        if (l1_pf_victim->isPrefetched()) {
                            multilevelstats.uselessPrefetches++;
                            l1_pf_victim->setPrefetched(false);
                        }

                        // Insert prefetched entry
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

        // ---------------------------------------------------------------------
        // Policy 5 (Markov to L1): Continue prefetch chain on L1 prefetch hit
        // Prefetch the most frequent successor to L1
        // ---------------------------------------------------------------------
        if (l1PrefetchPolicy == 5) {
            prefetchMarkovSuccessor(tid, instPC, true, 1);  // Prefetch 1 to L1
        }

    // -------------------------------------------------------------------------
    // Case 2: Hit on entry installed from pBuffer (Policy 4)
    // -------------------------------------------------------------------------
    } else if (l1_entry->isFromPBuffer()) {
        l1_entry->setFromPBuffer(false);  // Only count first reuse
    }

    DPRINTF(BTB, "L1 BTB hit for PC %#x, latency=%d cycles\n",
            instPC, l1Latency + extraLatency);

    // -------------------------------------------------------------------------
    // Check prediction match for prefetched entries
    // -------------------------------------------------------------------------
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

    return BTBLookupResult(l1_entry->target.get(), l1Latency + extraLatency,
                           true, false, false, isPrefetchHit, predMatch);
}


BTBLookupResult
MultiLevelBTB::handlePBufferHit(ThreadID tid, Addr instPC,
                                BTBEntry *pB_entry, bool taken)
{
    // -------------------------------------------------------------------------
    // Update prefetch statistics
    // -------------------------------------------------------------------------
    Cycles extraLatency = Cycles(0);
    if (pB_entry->isPrefetched()) {
        multilevelstats.prefetchHits++;
        pB_entry->setPrefetched(false);
        Cycles delta = curCycle() - pB_entry->getTimestamp();
        if (delta < l2Latency) {
            extraLatency = l2Latency - delta;
        }
    }

    // Track prefetch distance statistics (only for Policy 4, not Policy 6)
    uint8_t prefetchDistance = pB_entry->getPrefetchDistance();
    if ((l1PrefetchPolicy == 4 || l1PrefetchPolicy == 11) && prefetchDistance < 16) {
        multilevelstats.prefetchDistUsed[prefetchDistance]++;
    }

    // -------------------------------------------------------------------------
    // Promote entry from pBuffer to L1
    // -------------------------------------------------------------------------
    BTBEntry *l1_victim = l1btb.findVictim({instPC, tid});

    // Track if we're evicting an entry that was previously installed from pBuffer
    if (l1_victim->isFromPBuffer()) {
        multilevelstats.l1InstalledEvicted++;
    }

    l1btb.insertEntry({instPC, tid}, l1_victim);
    l1_victim->update(*pB_entry->target, pB_entry->inst);
    l1_victim->setFromPBuffer(true);  // Mark for reuse tracking
    // Only set prefetch distance for Policy 4 (spatial prefetch)
    if (l1PrefetchPolicy == 4 || l1PrefetchPolicy == 11) {
        l1_victim->setPrefetchDistance(prefetchDistance);
    }
    l1_victim->setPredTaken(pB_entry->getPredTaken());
    l1_victim->setPrefetched(false);

    // This entry is now useless since it has been promoted to L1
    pBuffer.invalidate(pB_entry);

    multilevelstats.l1Installed++;

    // -------------------------------------------------------------------------
    // Check prediction match
    // -------------------------------------------------------------------------
    bool predMatch = false;
    // if (cPred) {
    //     multilevelstats.predChecks[prefetchDistance]++;
    //     if (pB_entry->getPredTaken() == taken) {
    //         if (prefetchDistance == 0) {
    //             predMatch = true;
    //         }
    //         multilevelstats.predMatches[prefetchDistance]++;
    //     }
    // }

    // -------------------------------------------------------------------------
    // Policy 6/7/8/9/10 (Markov to pBuffer): Continue prefetch chain on pBuffer hit
    // Prefetch the most frequent successor(s) to pBuffer
    // -------------------------------------------------------------------------
    if (l1PrefetchPolicy == 6 || l1PrefetchPolicy == 7 || l1PrefetchPolicy == 8) {
        prefetchMarkovSuccessor(tid, instPC, false, 1);  // Prefetch 1 successor
    } else if (l1PrefetchPolicy == 9) {
        prefetchMarkovSuccessor(tid, instPC, false, 2);  // Prefetch 2 successors
    } else if (l1PrefetchPolicy == 10) {
        prefetchMarkovSuccessor(tid, instPC, false, 3);  // Prefetch 3 successors
    }

    DPRINTF(BTB, "pBuffer hit for PC %#x, promoted to L1\n", instPC);
    return BTBLookupResult(l1_victim->target.get(), l1Latency + extraLatency,
                           false, true, false, true, predMatch);
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

    // -------------------------------------------------------------------------
    // Insert entry into L1
    // -------------------------------------------------------------------------
    BTBEntry *l1_victim = l1btb.findVictim({instPC, tid});
    if (l1_victim->isPrefetched()) {
        multilevelstats.uselessPrefetches++;
        l1_victim->setPrefetched(false);
    } else if (l1_victim->isFromPBuffer()) {
        multilevelstats.l1InstalledEvicted++;
        l1_victim->setFromPBuffer(false);
    }
    l1btb.insertEntry({instPC, tid}, l1_victim);
    l1_victim->update(*l2_entry->target, l2_entry->inst);

    // -------------------------------------------------------------------------
    // Determine if we should prefetch (forward-only filtering)
    // -------------------------------------------------------------------------
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

        size_t numBranches = 0;
        for (Addr offset = minInstSize; offset <= endOffset; offset += minInstSize) {
            Addr pfAddr = instPC + offset;
            BTBEntry *l2_pf = l2btb.findEntry({pfAddr, tid});
            if (!l2_pf) continue;

            BTBEntry *l1_pf_check = l1btb.findEntry({pfAddr, tid});
            if (l1_pf_check) continue;

            multilevelstats.totalPrefetches++;
            numBranches++;

            // Policy 4: Prefetch to pBuffer
            if (l1PrefetchPolicy == 4 || l1PrefetchPolicy == 11) {
                if (pBuffer.findEntry({pfAddr, tid})) continue;
                BTBEntry *pB_victim = pBuffer.findVictim({pfAddr, tid});
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
            }
            // Policy 1/2/3: Prefetch to L1
            else {
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
            }
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
        prefetchMarkovSuccessor(tid, instPC, true, 1);   // Prefetch 1 to L1
    } else if (l1PrefetchPolicy == 6 || l1PrefetchPolicy == 7 || l1PrefetchPolicy == 8) {
        prefetchMarkovSuccessor(tid, instPC, false, 1);  // Prefetch 1 to pBuffer
    } else if (l1PrefetchPolicy == 9) {
        prefetchMarkovSuccessor(tid, instPC, false, 2);  // Prefetch 2 to pBuffer
    } else if (l1PrefetchPolicy == 10) {
        prefetchMarkovSuccessor(tid, instPC, false, 3);  // Prefetch 3 to pBuffer
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

    DPRINTF(BTB, "L2 BTB hit for PC %#x, latency=%d cycles, insert in L1\n",
            instPC, l2Latency);

    return BTBLookupResult(l2_entry->target.get(), l2Latency, false, false, true);
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
    }
}

void
MultiLevelBTB::prefetchMarkovSuccessor(ThreadID tid, Addr pc, bool toL1, unsigned numSuccessors)
{
    auto it = markovSuccessors.find(pc);
    if (it == markovSuccessors.end() || it->second.empty()) {
        return;  // No successor data for this PC
    }

    // Build a vector of (successor, frequency) pairs and sort by frequency
    std::vector<std::pair<Addr, uint64_t>> successors;
    for (const auto& [succ, freq] : it->second) {
        successors.push_back({succ, freq});
    }
    
    // Sort by frequency (descending)
    std::sort(successors.begin(), successors.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
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
        if (l1btb.findEntry({successor, tid})) {
            continue;
        }
        // Find in L2
        BTBEntry *l2_entry = l2btb.findEntry({successor, tid});
        if (!l2_entry) {
            continue;
        }
        
        // multilevelstats.markovDist.sample(successor - pc);
        multilevelstats.totalPrefetches++;

        if (toL1) {
            // Policy 5: Prefetch to L1
            BTBEntry *l1_victim = l1btb.findVictim({successor, tid});
            if (l1_victim->isPrefetched()) {
                multilevelstats.uselessPrefetches++;
                l1_victim->setPrefetched(false);
            }
            l1btb.insertEntry({successor, tid}, l1_victim);
            l1_victim->update(*l2_entry->target, l2_entry->inst);
            l1_victim->setPrefetched(true);
            l1_victim->setTimestamp(curCycle());
        } else {
            // Policy 6/7/8/9/10: Prefetch to pBuffer
            if (pBuffer.findEntry({successor, tid})) continue; 
            BTBEntry *pB_victim = pBuffer.findVictim({successor, tid});
            if (pB_victim->isPrefetched()) {
                multilevelstats.uselessPrefetches++;
            }
            pBuffer.insertEntry({successor, tid}, pB_victim);
            pB_victim->update(*l2_entry->target, l2_entry->inst);
            pB_victim->setPrefetched(true);
            pB_victim->setTimestamp(curCycle());
        }
        
        prefetched++;
    }
}

// trainMarkovOnCommit: Train Markov predictor on committed branches (Policy 7/8/9/10)
void
MultiLevelBTB::trainMarkovOnCommit(ThreadID tid, Addr pc, bool wasL2Hit)
{
    if (l1PrefetchPolicy != 7 && l1PrefetchPolicy != 8 && 
        l1PrefetchPolicy != 9 && l1PrefetchPolicy != 10) {
        return;
    }

    if(l1PrefetchPolicy == 7 && !wasL2Hit) {
        return;
    }

    if (prevBranchPC[tid] != 0) {
        markovSuccessors[prevBranchPC[tid]][pc]++; 
    }
    prevBranchPC[tid] = pc;
}

} // namespace gem5::branch_prediction