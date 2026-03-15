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
                                      bool taken = true,
                                      Addr blockStartAddr = 0) override;
    
    void update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                BranchType type = BranchType::NoBranch,
                StaticInstPtr inst = nullptr) override;
    
    const StaticInstPtr getInst(ThreadID tid, Addr instPC) override;

    void setBranchPredictor(ConditionalPredictor *cp) { cPred = cp; }

    void setBBMap(const std::unordered_map<Addr, Addr> *map) { bbMap_ = map; }

    void trainMarkovOnCommit(ThreadID tid, Addr pc, Addr startAddr,
                             Addr targetAddr, unsigned instSize, bool wasL2Hit);
    void trainPrefetchBitsOnCommit(ThreadID tid, Addr pc, bool actuallyTaken, BranchType type);
   
  private:
    ConditionalPredictor *cPred = nullptr;
    const std::unordered_map<Addr, Addr> *bbMap_ = nullptr;

    AssociativeCache<BTBEntry> l1btb;
  
    AssociativeCache<BTBEntry> pBuffer;

    /** In-flight prefetch : entries waiting for their arrival cycle. */
    AssociativeCache<BTBEntry> prefetchQueue;
    
    AssociativeCache<BTBEntry> l2btb;
    
    const Cycles l1Latency;
    const Cycles l2Latency;
    const unsigned minInstSize;
    const unsigned l1PrefetchPolicy;
    const bool trainBitsOnLookup;
    const bool trainBitsOnCommit;
    const bool togetherArrive;
    const bool prefetchBothForCall;
    const bool prefetchOnL1Hit;
    const bool prefetchOnPrefetchHit;
    const bool cleanBitsOnL1Promotion;
    const bool noPrefetchLatency;
    const unsigned prefetchDepth;
    const bool prefetchOnlyForward;
    const bool finalMarkov;
    const bool prefetchAllMarkovSuccessors;

    unsigned currentQueueSize;
    const unsigned maxPrefetchQueueSize;

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
        statistics::Scalar shadowPrefetches;
        statistics::Scalar prefetchQueueFull;
        
        // Unified prefetch coverage (for policy 4: pBuffer hits + L1 reuse)
        statistics::Scalar prefetchHits;            // pBuffer hit + L1 reuse (both are useful)
        statistics::Vector latePrefetchByPBHit;
        statistics::Vector latePrefetchByL2Hit;
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

        // Policy 13/14: commit-time training stats
        statistics::Scalar trainBitsL1Miss;  // branch not in L1 at commit, found in L2

        // Policy 12/13/14: prefetch direction counts
        statistics::Scalar takenPathPrefetches;     // prefetches triggered by prefetchTarget bit
        statistics::Scalar notTakenPathPrefetches;  // prefetches triggered by prefetchThrough bit

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

    // trainBitsOnLookup: per-thread tracking of previous block info for lookup training
    struct PrevBlockInfo {
        Addr branchPC = 0;
        Addr target = 0;
        Addr fallThrough = 0;
        bool valid = false;
    };
    std::vector<PrevBlockInfo> prevBlockInfo;

    // finalMarkov: per-thread tracking of previous committed branch exits
    struct PrevCommitBlockInfo {
        Addr branchPC = 0;
        Addr target = 0;
        Addr fallThrough = 0;
        bool valid = false;
    };
    std::vector<PrevCommitBlockInfo> prevCommitBlockInfo;

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
    void prefetchMarkovSuccessor(ThreadID tid, Addr pc, bool toL1,
                                 unsigned numSuccessors = 1,bool triggeredByPBHit=false,
                                 Cycles baseLatency = Cycles(0));

    // Policy 11: Shadow structures to simulate Markov prefetcher behavior alongside Spatial
    AssociativeCache<BTBEntry> shadowL1BTB;
    AssociativeCache<BTBEntry> shadowPBuffer;

    void performShadowLookup(ThreadID tid, Addr instPC, BranchType type);

    void handleShadowL2Hit(ThreadID tid, Addr instPC, BTBEntry* l2_entry, BranchType type);

    void prefetchShadowMarkovSuccessor(ThreadID tid, Addr pc, unsigned numSuccessors, BranchType predType);

    /** Drain arrived entries from prefetchQueue into pBuffer / L1 BTB. */
    void processPrefetchQueue(ThreadID tid);

    /** Enqueue a prefetch into the in-flight queue. */
    void enqueuePrefetch(Addr pc, ThreadID tid, BTBEntry *l2_entry,
                         Cycles arrivalCycle, bool toL1,
                         bool triggeredByPBHit, uint8_t pfDistance = 0, bool takenPrefetched=false);


    /** Returns true for policies that use prefetch-bit logic. */
    bool usesPrefetchBitPolicy() const;

    /** Write back prefetch bits from an L1 victim to its L2 copy. */
    void writeBackPrefetchBits(ThreadID tid, BTBEntry *l1_victim);

    /** Record previous block info for next-iteration training. */
    void recordPrevBlockInfo(ThreadID tid, Addr instPC, Addr targetAddr);

    /**
     * Prefetch a successor branch via bbMap lookup → pBuffer insertion.
     * @param lookupAddr  Address to look up in bbMap (target or fallThrough).
     * @param isTakenPath true → takenPathPrefetches stat; false → notTakenPathPrefetches.
     * @param baseLatency Cumulative latency for depth > 1 prefetches.
     */
    void prefetchViaBBMap(ThreadID tid, Addr lookupAddr, bool isTakenPath,
                          bool triggeredByPBHit, int depth=1,
                          Cycles baseLatency = Cycles(0));

};
} // namespace gem5::branch_prediction

#endif // __CPU_PRED_MULTILEVEL_BTB_HH__
