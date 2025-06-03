#include "cpu/pred/br_recycling.hh"

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/logging.hh"
#include "base/sat_counter.hh"
#include "base/trace.hh"
#include "cpu/pred/branch_type.hh"
#include "debug/Fetch.hh"
#include "debug/RecycledEntry.hh"

namespace gem5
{
namespace branch_prediction
{

// Helper: recycle only for indirect branches
static inline bool isIndirect(BranchType t)
{
    return t == BranchType::IndirectUncond ||
           t == BranchType::IndirectCond   ||
           t == BranchType::CallIndirect;
}

//Constructor

BranchRecyclingCache::BranchRecyclingCache(const Params &p)
  : IndirectPredictor(p),
    _instShiftAmt(p.instShiftAmt),
    _speculativeHistUpdate(p.speculativeHistUpdate),

    // BRP params
    mBits(14),                  // m
    nShift(7),                  // n = m/2
    brpSize(16384),             // 16K 2-bit counters
    brp(brpSize, SatCounter8(2)),
    ghr(p.numThreads, 0),

    lastMBpc(0),

  //Data Structures
    branchRecycleBuffer(),
    branchRecycleCache(),
    table()
{
    // Initialize each BRP counter to 1 (weakly "use")
    for (auto &c : brp) ++c;

    DPRINTF(RecycledEntry, "BRC: constructed (BRP=%u, m=%u, n=%u)\n",
            brpSize, mBits, nShift);
}

void
BranchRecyclingCache::reset()
{
    branchRecycleBuffer.clear();
    branchRecycleCache.clear();
    lastMBpc = 0;

    brp.assign(brpSize, SatCounter8(2));
    for (auto &c : brp) ++c;
}

//Hash according to paper

inline unsigned
BranchRecyclingCache::brpIndex(ThreadID tid, Addr cb_pc) const
{

    const uint64_t mask = (mBits >= 31) ? 0xffffffffu : ((1u << mBits) - 1u);

    const uint64_t bhr = static_cast<uint64_t>(ghr[tid]) & mask;
    const uint64_t cb  = static_cast<uint64_t>(cb_pc >> _instShiftAmt) & mask;
    const uint64_t mb  =
        static_cast<uint64_t>((lastMBpc >> _instShiftAmt) << nShift) & mask;

    const uint64_t x = bhr ^ cb ^ mb;
    return (x & (brpSize - 1));
}


//Function to check if stored Wron-Path-Outcome should be used
inline bool
BranchRecyclingCache::brpSaysUse(ThreadID tid, Addr cb_pc) const
{
    // 2-bit counter: use recycled outcome if counter >= 2
    return ( brp[brpIndex(tid, cb_pc)] > 1);
}

inline void
BranchRecyclingCache::brpTrain(ThreadID tid, Addr cb_pc, bool good)
{
    auto &c = brp[brpIndex(tid, cb_pc)];
    if (good) ++c; else --c;
}

//Lookup to and try to recycle Wrong-Path-Outcome

const PCStateBase *
BranchRecyclingCache::lookup(ThreadID tid, InstSeqNum sn, Addr pc,
                             void *&indirect_history)
{
    indirect_history = nullptr; // default

    // 1) Try recycled wrong-path outcomes first (FIFO per-PC),
    // BUT only if BRP allows
    auto wrongPathOutcome = branchRecycleCache.find(pc);
    if (wrongPathOutcome != branchRecycleCache.end() &&
     !wrongPathOutcome->second.empty() && brpSaysUse(tid, pc)) {
        auto &q = wrongPathOutcome->second;

        //TODO: check which in the list should be used
        const Entry &re = q.front();

        // Allocate history only when we return something
        auto *h = new History();
        h->pc            = pc;
        h->usedRecycle   = true;
        h->recycledTaken = re.taken;
        h->actualKnown   = false;
        h->brType        = re.brType;

        if (re.target)
            h->predTarget.reset(re.target->clone());

        indirect_history = h;

        q.pop_front();
        if (q.empty())
            branchRecycleCache.erase(wrongPathOutcome);

        //DPRINTF(RecycledEntry, "BRC.lookup recycled pc=%#x\n", pc);
        return h->predTarget.get(); // may be nullptr for direction-only
    }

    // Fall back to last committed indirect-target table
    auto it2 = table.find(pc);
    if (it2 != table.end()) {
        auto *h = new History();
        h->pc            = pc;
        h->usedRecycle   = false;
        h->recycledTaken = false;
        h->actualKnown   = false;
        h->brType        = BranchType::IndirectUncond;
        h->predTarget.reset(it2->second->clone());
        indirect_history = h;

        //DPRINTF(RecycledEntry, "BRC.lookup committed pc=%#x\n", pc);
        return h->predTarget.get();
    }

    return nullptr;
}

/* ------------------ update (capture outcomes) ------------------ */

void
BranchRecyclingCache::update(ThreadID tid, InstSeqNum sn, Addr pc,
                             bool squash, bool taken,
                             const PCStateBase &target,
                             BranchType brType, void *&indirect_history)
{
    // Ensure a history exists so commit can train BRP if needed
    auto *h = static_cast<History*>(indirect_history);
    if (!h) {
        h = new History();
        h->pc = pc;
        indirect_history = h;
    }
    h->brType = brType;

    if (squash) {
        // Wrong-path outcome → capture into write buffer
        Entry e;
        e.pc         = pc;
        e.hasOutcome = true;
        e.taken      = taken;
        e.brType     = brType;
        if (isIndirect(brType))
            e.target.reset(target.clone());
        branchRecycleBuffer.emplace_back(std::move(e));
        return;
    }

    // Correct-path resolution
    // remember actual outcome (for BRP training + table)
    h->actualKnown = true;
    h->actualTaken = taken;
    if (isIndirect(brType))
        h->actualTarget.reset(target.clone());
    else
        h->actualTarget.reset();

    // Update GHR (direction history); for taken-only, guard with if (taken)
    const unsigned mask = (mBits >= 31) ? 0xffffffffu : ((1u << mBits) - 1u);
    ghr[tid] = ((ghr[tid] << 1) | (h->actualTaken ? 1u : 0u)) & mask;
}

//Sqaush

void
BranchRecyclingCache::
squash(ThreadID tid, InstSeqNum sn, void *&indirect_history)
{
    if (indirect_history) {
        auto *h = static_cast<History*>(indirect_history);
        // Record last mispredicted branch PC component for hashing
        lastMBpc = h->pc;
        delete h;
        indirect_history = nullptr;
    }

    if (!branchRecycleBuffer.empty()) {
        // Move all wrong-path entries into per-PC recycled queues (FIFO)
        for (auto &e : branchRecycleBuffer)
            branchRecycleCache[e.pc].emplace_back(std::move(e));
        branchRecycleBuffer.clear();

        DPRINTF(RecycledEntry, "BRC.squash published recycled entries\n");
    }
}

//Commit, train BRP and update

void
BranchRecyclingCache::
commit(ThreadID tid, InstSeqNum sn, void *&indirect_history)
{
    auto *h = static_cast<History*>(indirect_history);
    if (!h) return;

    if (h->usedRecycle && h->actualKnown) {
        bool good = true;

        // If indirect: compare predicted vs actual targets
        if (h->predTarget && h->actualTarget) {
            good = (h->predTarget->instAddr() == h->actualTarget->instAddr());
        }
        // If no target on either side -> direction-only recycle
        else if (!h->predTarget && !h->actualTarget) {
            good = (h->recycledTaken == h->actualTaken);
        }

        brpTrain(tid, h->pc, good);
        if (!good) {
            // On a misprediction, record this branch as the last mispredicted
            lastMBpc = h->pc;
        }
        DPRINTF(RecycledEntry,
            "BRC.commit train pc=%#x good=%d\n", h->pc, good);
    }

    // Populate/update the committed target table for indirects
    if (h->actualKnown && isIndirect(h->brType) && h->actualTarget) {
        table[h->pc] = std::unique_ptr<PCStateBase>(h->actualTarget->clone());
    }

    delete h;
    indirect_history = nullptr;
}

} // namespace branch_prediction
} // namespace gem5
