#ifndef __CPU_PRED_BR_RECYCLING_HH__
#define __CPU_PRED_BR_RECYCLING_HH__

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/pred/conditional.hh"
#include "cpu/pred/tage_sc_l.hh"
#include "params/BranchRecyclingCache.hh"

namespace gem5
{
namespace branch_prediction
{

class BranchRecyclingCache : public ConditionalPredictor
{
  public:
    using Params = BranchRecyclingCacheParams;
    BranchRecyclingCache(const Params &p);

    bool lookup(ThreadID tid, Addr pc, void * &bp_history) override;

    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                           void * &bpHistory) override;

    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void * &bp_history) override;

    void update(ThreadID tid, Addr pc, bool taken,
                void * &bp_history, bool squashed,
                const StaticInstPtr & inst, Addr target) override;

    void squash(ThreadID tid, void * &bp_history) override;

  private:
    //struct for capturing wrong paths
    struct Entry
    {
        Addr pc = 0;
        bool hasOutcome = false;
        bool taken = false;
        BranchType brType = BranchType::NoBranch;
    };
//History struct for branches
    struct History
    {
        Addr pc = 0;


        bool usedRecycle = false;
        bool recycledTaken = false;


        bool actualKnown = false;
        bool actualTaken = false;

        BranchType brType = BranchType::DirectCond;

        unsigned brpIdx = 0;

        bool base_pred = false;
        void* tage_bi = nullptr;
    };

    TAGE_SC_L* base;
    const bool enableRecycling;

    //Paper parameters
    unsigned mBits;
    unsigned nShift;
    unsigned brpSize;                 // number of 2-bit counters
    std::vector<SatCounter8> stateMachines;     // 2-bit counters
    std::vector<uint64_t>    ghr;     // per-thread m-bit history
    Addr lastMBpc = 0;                // last mispredicted branch PC

    unsigned stateMachineIdx(ThreadID tid, Addr cb_pc) const;
    bool     brpSaysUse(ThreadID tid, Addr cb_pc) const;
    void     brpTrainByIdx(unsigned idx, bool good);

    // Per-PC recycled entries
    std::unordered_map<Addr, std::deque<Entry>> branchRecycleCache;


    // Debug counters + function
    uint64_t recycledCount = 0;
    uint64_t recycledNotTaken = 0;

    void dumpFinalDebugCounters();
};

} // namespace branch_prediction
} // namespace gem5

#endif
