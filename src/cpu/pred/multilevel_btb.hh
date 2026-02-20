#ifndef __CPU_PRED_MULTILEVEL_BTB_HH__
#define __CPU_PRED_MULTILEVEL_BTB_HH__

#include "base/cache/associative_cache.hh"
#include "cpu/pred/conditional.hh"
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

    void trainMarkovOnCommit(ThreadID tid, Addr pc, bool wasL2Hit);
   
  private:
    ConditionalPredictor *cPred = nullptr;

    AssociativeCache<BTBEntry> l1btb;
    /* Prefetch Buffer */
    AssociativeCache<BTBEntry> pBuffer;
    
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
        statistics::SparseHistogram markovDist;

        // Spatial locality statistics
        statistics::SparseHistogram dist1HistoryPC;
        statistics::SparseHistogram dist1HistoryTarget;
        statistics::SparseHistogram dist2HistoryPC;
        statistics::SparseHistogram dist2HistoryTarget;

        statistics::Scalar l1MissL2Hits;
        statistics::Scalar uselessPrefetches;
        statistics::Scalar totalPrefetches;
        
        // Unified prefetch coverage (for policy 4: pBuffer hits + L1 reuse)
        statistics::Scalar prefetchHits;            // pBuffer hit + L1 reuse (both are useful)
        statistics::Formula prefetchCoverage;       // prefetchHits / (prefetchHits + totalPrefetches)
        
        // Separate useless rates for pBuffer and L1 (policy 4)
        statistics::Formula pBufferUselessRate;     // uselessPrefetches / totalPrefetches
        
        statistics::Scalar l1InstalledEvicted;   // L1 evictions of pBuffer-installed entries without reuse
        statistics::Scalar l1Installed;             // count of entries installed from pBuffer to L1
        statistics::Formula l1UselessInstalledRate;  // l1InstalledEvicted / l1Installed

        statistics::Distribution numBranchesPerPrefetch;

        statistics::Vector prefetchDistCount;
        statistics::Vector prefetchDistUsed;
        statistics::Formula prefetchUsefulness;
        // For TAGE prediction
        // statistics::Scalar predMatches;
        // statistics::Scalar predChecks;
        statistics::Vector predMatches;
        statistics::Vector predChecks;
        statistics::Formula predMatchRatio;

        // Policy 11: Shadow Statistics (indexed by BranchType)
        statistics::Vector shadowOverlaps;    // Hit in BOTH Real and Shadow
        statistics::Vector spatialOnlyHits;   // Hit in Real, Miss in Shadow
        statistics::Vector markovOnlyHits;    // Miss in Real, Hit in Shadow
        statistics::Vector uniMisses;         // Miss in BOTH, but Hit in L2
        // Breakdown of markovOnlyHits by predecessor's BranchType
        // Index 0 (NoBranch) = demand fill from L2, not prefetched
        statistics::Vector markovOnlyHitsByPredType;

        MultiLevelBTB *btb;
        
    } multilevelstats;
    // Markov prefetcher data structure
    // key: branch PC (L1 miss, L2 hit)
    // value: map of successor PC -> access frequency
    std::unordered_map<Addr, std::unordered_map<Addr, uint64_t>> markovSuccessors;

    struct BranchInfo {
        Addr pc;
        Addr target;
    };
    // History of L1 miss L2 hit branches per thread
    std::vector<std::deque<BranchInfo>> l1MissL2HitHistory;

    // tacking the previous branch PC for each thread.
    std::vector<Addr> prevBranchPC;
    // Shadow previous branch PC for Policy 11 training
    std::vector<Addr> shadowPrevBranchPC;
    friend struct MultiLevelBTBStats;

    /**
     * Handle L1 BTB hit.
     * Handles prefetched entry hits, Policy 3 next-region prefetching,
     * and prediction match checking.
     */
    BTBLookupResult handleL1Hit(ThreadID tid, Addr instPC, BTBEntry *l1_entry,
                                bool taken);

    /**
     * Handle pBuffer hit (Policy 4 only).
     * Promotes entry from pBuffer to L1 and tracks statistics.
     */
    BTBLookupResult handlePBufferHit(ThreadID tid, Addr instPC,
                                     BTBEntry *pB_entry, bool taken);

    /**
     * Handle L2 BTB hit.
     * Inserts entry into L1, performs prefetching based on policy,
     * and tracks successor relationships.
     */
    BTBLookupResult handleL2Hit(ThreadID tid, Addr instPC, BTBEntry *l2_entry,
                                BranchType type, bool taken);

    /**
     * Prefetch the most frequent successors of given PC (Policy 5/6/7/8/9/10).
     * @param numSuccessors Number of top successors to prefetch (default 1)
     */
    void prefetchMarkovSuccessor(ThreadID tid, Addr pc, bool toL1, unsigned numSuccessors = 1);

    // Policy 11: Shadow structures to simulate Markov prefetcher behavior alongside Spatial
    AssociativeCache<BTBEntry> shadowL1BTB;
    AssociativeCache<BTBEntry> shadowPBuffer;

    void performShadowLookup(ThreadID tid, Addr instPC, BranchType type);

    void handleShadowL2Hit(ThreadID tid, Addr instPC, BTBEntry* l2_entry, BranchType type);

    void prefetchShadowMarkovSuccessor(ThreadID tid, Addr pc, unsigned numSuccessors, BranchType predType);

};
} // namespace gem5::branch_prediction

#endif // __CPU_PRED_MULTILEVEL_BTB_HH__
