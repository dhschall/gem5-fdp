#ifndef __CPU_PRED_MULTILEVEL_BTB_HH__
#define __CPU_PRED_MULTILEVEL_BTB_HH__

#include "base/cache/associative_cache.hh"
#include "cpu/pred/btb.hh"
#include "cpu/pred/btb_entry.hh"
#include "params/MultiLevelBTB.hh"
#include <deque>
#include <vector>

namespace gem5::branch_prediction
{


class MultiLevelBTB : public BranchTargetBuffer
{
  public:
    MultiLevelBTB(const MultiLevelBTBParams &params);

    void memInvalidate() override;
    bool valid(ThreadID tid, Addr instPC) override;
    
    const PCStateBase *lookup(ThreadID tid, Addr instPC,
                              BranchType type = BranchType::NoBranch) override;
    
    BTBLookupResult lookupWithLatency(ThreadID tid, Addr instPC,
                                      BranchType type = BranchType::NoBranch,
                                      bool taken = true) override;
    
    void update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                BranchType type = BranchType::NoBranch,
                StaticInstPtr inst = nullptr) override;
    
    const StaticInstPtr getInst(ThreadID tid, Addr instPC) override;

    void setBranchPredictor(ConditionalPredictor *cp) { cPred = cp; }
   
  private:
    ConditionalPredictor *cPred = nullptr;

    BTBEntry *findL1Entry(Addr instPC, ThreadID tid);
    
    BTBEntry *findL2Entry(Addr instPC, ThreadID tid);

    AssociativeCache<BTBEntry> l1btb;
    
    AssociativeCache<BTBEntry> l2btb;
    
    const Cycles l1Latency;
    const Cycles l2Latency;
    const unsigned minInstSize;
    const unsigned l1PrefetchPolicy;
    const bool prefetchOnlyForward;

    // Multi-level BTB specific statistics
    struct MultiLevelBTBStats : public statistics::Group
    {
        MultiLevelBTBStats(statistics::Group *parent, MultiLevelBTB *btb);
        
        void preDumpStats() override;
        //For exploring the 1-history Markov prefetcher:
        //If branch A misses in L1BTB1(hits in L2BTB), 
        //then the immediate branch B is the successor of A if B also misses in L1BTB2 (hits in L2BTB2).
        statistics::SparseHistogram successorCountDist;

        // Spatial locality statistics
        statistics::SparseHistogram dist1HistoryPC;
        statistics::SparseHistogram dist1HistoryTarget;
        statistics::SparseHistogram dist2HistoryPC;
        statistics::SparseHistogram dist2HistoryTarget;

        statistics::Scalar l1PrefetchHits;
        statistics::Scalar l1MissL2Hits;
        statistics::Scalar uselessPrefetches;
        statistics::Scalar totalPrefetches;
        statistics::Formula l1PrefetchCoverage;
        statistics::Formula uselessPrefetchRate;

        statistics::Distribution numBranchesPerPrefetch;
        // For TAGE prediction
        statistics::Scalar predMatches;
        statistics::Scalar predChecks;
        statistics::Formula predMatchRatio;

        statistics::Vector prefetchDistCount;
        statistics::Vector prefetchDistUsed;
        statistics::Formula prefetchUsefulness;

        MultiLevelBTB *btb;
        
    } multilevelstats;
    // key: static branch PC (L1 miss, L2 hit)
    // value: set of successor branch PCs (also L1 miss, L2 hit)
    std::unordered_map<Addr, std::set<Addr>> l1MissL2HitSuccessors;

    struct BranchInfo {
        Addr pc;
        Addr target;
    };
    // History of L1 miss L2 hit branches per thread
    std::vector<std::deque<BranchInfo>> l1MissL2HitHistory;

    // tacking the previous branch PC for each thread.
    std::vector<Addr> prevBranchPC;
    friend struct MultiLevelBTBStats;
};
} // namespace gem5::branch_prediction

#endif // __CPU_PRED_MULTILEVEL_BTB_HH__
