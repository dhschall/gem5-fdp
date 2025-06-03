#ifndef __CPU_PRED_BR_RECYCLING_HH__
#define __CPU_PRED_BR_RECYCLING_HH__

#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/pred/indirect.hh"
#include "params/BranchRecyclingCache.hh"

namespace gem5
{
class PCStateBase;

namespace branch_prediction
{

class BranchRecyclingCache : public IndirectPredictor
{
  public:
    using Params = BranchRecyclingCacheParams;
    BranchRecyclingCache(const Params &p);

    // IndirectPredictor
    const PCStateBase* lookup(ThreadID tid, InstSeqNum sn, Addr pc,
                              void *&indirect_history) override;
    void update(ThreadID tid, InstSeqNum sn, Addr pc, bool squash, bool taken,
                const PCStateBase &target, BranchType brType,
                void *&indirect_history) override;
    void squash(ThreadID tid, InstSeqNum sn, void *&indirect_history) override;
    void commit(ThreadID tid, InstSeqNum sn, void *&indirect_history) override;
    void reset() override;

  private:
    /* ---------- Wrong-path capture entry ---------- */
    struct Entry
    {
        Addr pc = 0;
        bool hasOutcome = false;
        bool taken = false;
        BranchType brType = BranchType::NoBranch;
        // Only valid for indirects:
        std::unique_ptr<PCStateBase> target;
    };

    /* ---------- Per-branch call history between callbacks ---------- */
    struct History
    {
        Addr pc = 0;
        BranchType brType = BranchType::NoBranch;

        // What we predicted at lookup
        std::unique_ptr<PCStateBase> predTarget;

        // If a recycled prediction was used:
        bool usedRecycle = false;
        bool recycledTaken = false;

        // The actual correct path outcome (filled in update when resolved)
        bool actualKnown = false;
        bool actualTaken = false;
        std::unique_ptr<PCStateBase> actualTarget;
    };

    /* ---------- Parameters copied from SimObject ---------- */
    const unsigned _instShiftAmt;
    const bool     _speculativeHistUpdate;

    /* ---------- BRP (When-to-recycle predictor) ---------- */
    // Paper defaults: m = 14, n = m/2, 16K 2-bit counters
    unsigned mBits;
    unsigned nShift;
    // number of 2-bit counters
    unsigned brpSize;
    // 2-bit counters
    std::vector<SatCounter8> brp;
    // per-thread m-bit history
    std::vector<uint64_t>    ghr;
    // last mispredicted branch PC (folded)
    Addr lastMBpc = 0;

    unsigned brpIndex(ThreadID tid, Addr cb_pc) const;
    bool     brpSaysUse(ThreadID tid, Addr cb_pc) const;
    void     brpTrain(ThreadID tid, Addr cb_pc, bool good);

    // Filled while fetching current speculative path moved on squash,
    //prevents entries from being used before they are published
    std::deque<Entry> branchRecycleBuffer;

    // Per-PC recycled entries (FIFO)
    std::unordered_map<Addr, std::deque<Entry>> branchRecycleCache;

    // Fallback: last committed indirect targets
    std::unordered_map<Addr, std::unique_ptr<PCStateBase>> table;
};

} // namespace branch_prediction
} // namespace gem5

#endif
