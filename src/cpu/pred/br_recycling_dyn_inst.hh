#ifndef __CPU_PRED_BR_RECYCLING_DYN_INST_HH__
#define __CPU_PRED_BR_RECYCLING_DYN_INST_HH__

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/pred/conditional.hh"
#include "cpu/pred/tage_sc_l.hh"
#include "params/BranchRecyclingCacheDynInst.hh"
#include "sim/probe/probe.hh"

namespace gem5
{
namespace branch_prediction
{

class BranchRecyclingCacheDynInst : public ConditionalPredictor
{
  public:
    using Params = BranchRecyclingCacheDynInstParams;
    BranchRecyclingCacheDynInst(const Params &p);

    bool lookup(ThreadID tid, Addr pc, void *&bp_history) override;

    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                           void *&bpHistory) override;

    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void *&bp_history) override;

    void update(ThreadID tid, Addr pc, bool taken, void *&bp_history,
                bool squashed, const StaticInstPtr &inst,
                Addr target) override;

    void squash(ThreadID tid, void *&bp_history) override;

    void regProbeListeners() override;

    void dump(const std::string &filename);

    void
    setCPU(BaseCPU *_cpu)
    {
        cpu = _cpu;
    }

  private:
    // Branch outcome stored in per-PC bucket
    struct Entry
    {
        Addr pc = 0;
        bool taken = false;
        BranchType brType = BranchType::NoBranch;
        InstSeqNum seqNum = 0;
        bool valid = true;
    };

    struct RecyclingBucket
    {
        // Training
        int execs = 0;
        int mispredictsTage = 0;
        int mispredictsRecycle = 0;
        int trainingMetric = 0;
        bool useRecycle = false;

        // Stride DynAddr
        Addr lastDynAddr = 0;
        int lastDynAddrOffset = 0;
        int strideCounterDynAddr = 0;

        // FIFO queue of outcomes
        std::deque<Entry> FIFO_queue;

        void
        reset()
        {
            execs = 0;
            mispredictsTage = 0;
            mispredictsRecycle = 0;
            useRecycle = false;

            lastDynAddr = 0;
            lastDynAddrOffset = 0;
            strideCounterDynAddr = 0;

            FIFO_queue.clear();
        }

        void
        pushFifoCapped(const Entry &e, size_t bucketSize)
        {
            if (FIFO_queue.size() >= bucketSize) {
                FIFO_queue.pop_front(); // drop oldest
            }
            FIFO_queue.push_back(e); // add newest
        }

        bool
        popFifo(Entry &out)
        {
            if (FIFO_queue.empty()) {
                return false;
            }
            out = FIFO_queue.front(); // oldest
            FIFO_queue.pop_front();
            return true;
        }
    };

    // Per-lookup history
    struct History
    {
        Addr pc = 0;

        bool usedRecycle = false;
        bool recycledTaken = false;

        bool actualKnown = false;
        bool actualTaken = false;

        BranchType brType = BranchType::DirectCond;

        bool base_pred = false;
        void *tage_bi = nullptr;
    };

    struct BucketNode
    {
        RecyclingBucket bucket;
        uint64_t freq = 0;  // LFU count
        uint64_t stamp = 0; // increasing counter for tie braker
    };

    class BucketCache
    {
      public:
        explicit BucketCache(size_t max_buckets) : maxBuckets(max_buckets) {}

        BucketNode *find(Addr pc);
        BucketNode *getOrAllocBucket(Addr pc);

        size_t
        size() const
        {
            return BRC.size();
        }

      private:
        size_t maxBuckets;
        uint64_t globalStamp = 0;

        std::unordered_map<Addr, BucketNode> BRC;

        void
        touch(BucketNode &n)
        {
            n.freq++;
            n.stamp = ++globalStamp;
        }

        void evictBucket();
    };

    // Base predictor
    TAGE_SC_L *base;
    const bool enableRecycling;

    // Stride helper
    InstSeqNum lastSeqNum = 0;

    // Configurable Parameters 1
    const bool enableTraining;
    const bool enableStride;

    // Wrapper with evict functions for BRC
    BucketCache bucketCache;

    // Configurable Parameters 2
    const int bucketSize;
    const int trainingInterval;
    const int strideConfidenceThreshold;
    // Probes
    ProbeListenerPtr<> listener;  // ToCommit
    ProbeListenerPtr<> slistener; // SquashInst
    ProbeListenerPtr<> blistener; // BranchDependency
    BaseCPU *cpu;

    // Probe handlers
    void notifyExecutedInst(const o3::DynInstPtr &inst);
    void notifySquashedInst(const o3::DynInstPtr &mispred_inst,
                            const o3::DynInstPtr &sq_inst);
    void notifyDependentInst(const o3::DynInstPtr &branch_i,
                             const o3::DynInstPtr &load_i);

    // Helpers
    void pushExecutedOutcome(const o3::DynInstPtr &inst);

    // Bucket helpers
    RecyclingBucket *findBucket(Addr pc);
    RecyclingBucket *getOrAllocBucket(Addr pc);

    // Per-PC stats
    struct branch_info
    {
        int exec = 0;
        int taken = 0;
        int mispred = 0;
        int biggestStride = 0;
    };
    std::unordered_map<Addr, branch_info> branchStats;

  public:
    struct BranchRecyclingCacheDynInstStats : public statistics::Group
    {
        BranchRecyclingCacheDynInstStats(statistics::Group *parent);
        statistics::Scalar recycledPred;
        statistics::Scalar basePred;
        statistics::Scalar RecycleBaseDiffer;
        statistics::Scalar misPredictedFaultyRecycle;
        statistics::Scalar missPredictedTageFault;
        statistics::Scalar misPredictedBothPredictorsWrong;
        statistics::Scalar missPredicts;
        statistics::Scalar committedCount;
    } stats;
};

} // namespace branch_prediction
} // namespace gem5

#endif
