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
      ADD_STAT(l1PrefetchHits, statistics::units::Count::get(), "Number of hits on prefetched L1 entries"),
      ADD_STAT(l1MissL2Hits, statistics::units::Count::get(), "Number of L1 misses that hit in L2"),
      ADD_STAT(uselessPrefetches, statistics::units::Count::get(), "Number of useless prefetches"),
      ADD_STAT(totalPrefetches, statistics::units::Count::get(), "Total number of prefetches"),
      ADD_STAT(l1PrefetchCoverage, statistics::units::Ratio::get(), "L1 Prefetch Coverage"),
      ADD_STAT(uselessPrefetchRate, statistics::units::Ratio::get(), "Useless L1 Prefetch Rate"),
      btb(btb)
{
    using namespace statistics;
    successorCountDist.init(0).flags(total | pdf);
    dist1HistoryPC.init(0).flags(total | pdf);
    dist1HistoryTarget.init(0).flags(total | pdf);
    dist2HistoryPC.init(0).flags(total | pdf);
    dist2HistoryTarget.init(0).flags(total | pdf);

    l1PrefetchHits.flags(total);
    l1MissL2Hits.flags(total);
    uselessPrefetches.flags(total);
    totalPrefetches.flags(total);

    l1PrefetchCoverage = l1PrefetchHits / (l1PrefetchHits + l1MissL2Hits);
    uselessPrefetchRate = uselessPrefetches / totalPrefetches;
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
      l2btb("l2BTB", p.l2NumEntries, p.l2Associativity,
            p.l2ReplPolicy, p.l2IndexingPolicy,
            BTBEntry(genTagExtractor(p.l2IndexingPolicy))),
      l1Latency(p.l1Latency),
      l2Latency(p.l2Latency),
      minInstSize(p.minInstSize),
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
}

BTBEntry *
MultiLevelBTB::findL1Entry(Addr instPC, ThreadID tid)
{
    return l1btb.findEntry({instPC, tid});
}

BTBEntry *
MultiLevelBTB::findL2Entry(Addr instPC, ThreadID tid)
{
    return l2btb.findEntry({instPC, tid});
}

bool
MultiLevelBTB::valid(ThreadID tid, Addr instPC)
{
    BTBEntry *l1_entry = l1btb.findEntry({instPC, tid});
    if (l1_entry != nullptr) {
        DPRINTF(BTB, "L1 BTB valid for PC %#x\n", instPC);
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
MultiLevelBTB::lookupWithLatency(ThreadID tid, Addr instPC, BranchType type)
{
    stats.lookups[type]++;

    // lookup l1 btb firstly
    BTBEntry *l1_entry = l1btb.accessEntry({instPC, tid});
    if (l1_entry != nullptr) {
        if (l1_entry->isPrefetched()) {
            multilevelstats.l1PrefetchHits++;
            l1_entry->setPrefetched(false);
        }
        
        DPRINTF(BTB, "L1 BTB hit for PC %#x, latency=%d cycles\n", instPC, l1Latency);
        return BTBLookupResult(l1_entry->target.get(), l1Latency, true, false);
    }

    // L1miss, lookup l2 btb
    BTBEntry *l2_entry = l2btb.accessEntry({instPC, tid});
    if (l2_entry != nullptr) {
        multilevelstats.l1MissL2Hits++;
        
        auto l1_victim = l1btb.findVictim({instPC, tid});
        if (l1_victim->isPrefetched()) {
            multilevelstats.uselessPrefetches++;
            l1_victim->setPrefetched(false);
        }
        l1btb.insertEntry({instPC, tid}, l1_victim);
        l1_victim->update(*l2_entry->target, l2_entry->inst);
        

        // Prefetching
        for (Addr offset = minInstSize; offset <= 128; offset += minInstSize) {
            Addr pfAddr = instPC + offset;
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
                    multilevelstats.totalPrefetches++;
                }
            }
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