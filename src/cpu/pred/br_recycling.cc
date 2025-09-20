#include "cpu/pred/br_recycling.hh"

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/callback.hh"
#include "base/logging.hh"
#include "base/sat_counter.hh"
#include "base/trace.hh"
#include "cpu/pred/branch_type.hh"
#include "debug/Fetch.hh"
#include "debug/RecycledEntry.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace branch_prediction
{

static inline bool isIndirect(BranchType t)
{
    return t == BranchType::IndirectUncond ||
           t == BranchType::IndirectCond   ||
           t == BranchType::CallIndirect;
}

BranchRecyclingCache::BranchRecyclingCache(const Params &p)
  : ConditionalPredictor(p),
    // BRP params as paper recomend
    mBits(14),
    nShift(7), //=m/2
    brpSize(16384),
    stateMachines(brpSize, SatCounter8(2)),
    ghr(p.numThreads, 0),
    lastMBpc(0),
    branchRecycleCache(),
    recycledCount(0),
    recycledNotTaken(0)
{
    for (auto &c : stateMachines) ++c;

    DPRINTF(RecycledEntry, "BRC: constructed (BRP=%u, m=%u, n=%u)\n",
            brpSize, mBits, nShift);

    gem5::registerExitCallback([this]() { this->dumpFinalDebugCounters(); });
}

inline unsigned
BranchRecyclingCache::stateMachineIdx(ThreadID tid, Addr cb_pc) const
{
    const uint64_t mask = (mBits >= 31) ? 0xffffffffu : ((1u << mBits) - 1u);
    const uint64_t bhr = static_cast<uint64_t>(ghr[tid]) & mask;
    const uint64_t cb  = static_cast<uint64_t>(cb_pc >> 2) & mask;
    const uint64_t mb  =
        static_cast<uint64_t>((lastMBpc >> 2) << nShift) & mask;

    const uint64_t x = bhr ^ cb ^ mb;
    return (x & (brpSize - 1));
}

inline bool
BranchRecyclingCache::brpSaysUse(ThreadID tid, Addr cb_pc) const
{
    return (stateMachines[stateMachineIdx(tid, cb_pc)] > 1);
}

inline void
BranchRecyclingCache::brpTrainByIdx(unsigned idx, bool good)
{
    auto &c = stateMachines[idx];
    if (good) ++c; else --c;
}

bool
BranchRecyclingCache::lookup(ThreadID tid, Addr pc, void * &bp_history)
{
    bp_history = nullptr;
    const unsigned idx_now = stateMachineIdx(tid, pc);

    auto it = branchRecycleCache.find(pc);
    DPRINTF(RecycledEntry, "BRC.lookup pc=%#x, brpSaysUse=%d\n",
            pc, (int)brpSaysUse(tid, pc));

    // Try to use a recycled outcome from BRC
    if (it != branchRecycleCache.end() && !it->second.empty()
        && brpSaysUse(tid, pc))
    {
        auto &q = it->second;
        const Entry &re = q.front();

        auto *h = new History();
        h->pc            = pc;
        h->usedRecycle   = true;
        h->recycledTaken = re.taken;
        h->actualKnown   = false;
        h->brType        = re.brType;
        h->brpIdx        = idx_now;

        bp_history = h;
        q.pop_front();

        recycledCount++;
        DPRINTF(RecycledEntry,
            "BRC.lookup pc=%#x recycled dir=%d (q=%zu) idx=%u\n",
            pc, (int)re.taken, q.size(), idx_now);

        return re.taken;
    }


    auto *h = new History();
    h->pc            = pc;
    h->usedRecycle   = false;
    h->recycledTaken = false;
    h->actualKnown   = false;
    h->brType        = BranchType::DirectCond;
    h->brpIdx        = idx_now;
    bp_history = h;

    DPRINTF(RecycledEntry, "BRC.lookup pc=%#x fallback dir=0 idx=%u\n",
            pc, idx_now);

    recycledNotTaken++;
    return false;
}

void
BranchRecyclingCache::branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                                        void * &bpHistory)
{
    if (bpHistory) return;
    auto *h = new History();
    h->pc          = pc;
    h->usedRecycle = false;
    h->actualKnown = false;
    h->brType      =
    uncond ? BranchType::DirectUncond : BranchType::DirectCond;
    h->brpIdx      = stateMachineIdx(tid, pc);
    bpHistory = h;

    DPRINTF(RecycledEntry, "BRC.placeholder pc=%#x idx=%u\n", pc, h->brpIdx);
}

void
BranchRecyclingCache::updateHistories(ThreadID tid, Addr pc, bool uncond,
                                      bool taken, Addr,
                                      const StaticInstPtr &inst,
                                      void * &bp_history)
{
    auto *h = static_cast<History*>(bp_history);
    if (!h) {
        h = new History();
        h->pc          = pc;
        h->usedRecycle = false;
        h->actualKnown = false;
        h->brpIdx      = stateMachineIdx(tid, pc);
        bp_history = h;
    }

    h->actualKnown = true;
    h->actualTaken = taken;

    if (inst) {
        if (inst->isIndirectCtrl())
            h->brType = uncond ? BranchType::IndirectUncond
                               : BranchType::IndirectCond;
        else if (inst->isUncondCtrl())
            h->brType = BranchType::DirectUncond;
        else
            h->brType = BranchType::DirectCond;
    }

    const unsigned mask =
        (mBits >= 31) ? 0xffffffffu : ((1u << mBits) - 1u);
    ghr[tid] = ((ghr[tid] << 1) | (taken ? 1u : 0u)) & mask;

    DPRINTF(RecycledEntry,
        "BRC.updateHistories pc=%#x taken=%d ghr=%#x\n",
        pc, (int)taken, (unsigned)ghr[tid]);
}
void
BranchRecyclingCache::update(ThreadID tid, Addr pc, bool taken,
                             void * &bp_history, bool squashed,
                             const StaticInstPtr &inst, Addr)
{
    auto *h = static_cast<History*>(bp_history);
    if (!h) {
        h = new History();
        h->pc          = pc;
        h->usedRecycle = false;
        h->actualKnown = true;
        h->actualTaken = taken;
        h->brpIdx      = stateMachineIdx(tid, pc);
        bp_history     = h;
    }

    h->actualKnown = true;
    h->actualTaken = taken;

    if (inst) {
        if (inst->isIndirectCtrl())
            h->brType = inst->isUncondCtrl() ? BranchType::IndirectUncond
                                             : BranchType::IndirectCond;
        else if (inst->isUncondCtrl())
            h->brType = BranchType::DirectUncond;
        else
            h->brType = BranchType::DirectCond;
    }

    if (squashed) {

        Entry e;
        e.pc         = pc;
        e.hasOutcome = true;
        e.taken      = taken;
        e.brType     = h->brType;

        auto &q = branchRecycleCache[pc];
        q.emplace_back(std::move(e));

        lastMBpc = pc;

        DPRINTF(RecycledEntry,
            "BRC.update SQUASH pc=%#x queued
            wrong-path dir=%d (q=%zu); lastMBpc=%#x\n",
            pc, (int)taken, q.size(), lastMBpc);


        delete h;
        bp_history = nullptr;
        return;
    }


    const bool baselineCorrect = !h->actualTaken;
    const bool wpEqCp = h->usedRecycle &&
    (h->recycledTaken == h->actualTaken);

    //Train how described in Paper
    if (h->usedRecycle) {
        if (!baselineCorrect && wpEqCp) {
            // If baseline mispredicted && WP==CP -> increment
            brpTrainByIdx(h->brpIdx, true);
            lastMBpc = pc; // record last mispredicted branch for hash
            DPRINTF(RecycledEntry,
                "BRC.update COMMIT pc=%#x BRP++
                (baseline wrong, WP==CP) cnt=%u lastMBpc=%#x\n",
                pc, (unsigned)stateMachines[h->brpIdx], lastMBpc);
        } else if (baselineCorrect && !wpEqCp) {
            // If baseline correct && WP!=CP -> decrement
            brpTrainByIdx(h->brpIdx, false);
            DPRINTF(RecycledEntry,
                "BRC.update COMMIT pc=%#x BRP--
                (baseline correct, WP!=CP) cnt=%u\n",
                pc, (unsigned)stateMachines[h->brpIdx]);
        } else {
            DPRINTF(RecycledEntry,
                "BRC.update COMMIT pc=%#x no BRP
                change (usedRecycle, but no rule hit)\n",
                pc);
        }
    } else {
        //Also train if WP wasn't used contrary to
        //paper otherwise training would never start
        if (!baselineCorrect) {
            brpTrainByIdx(h->brpIdx, true);
            lastMBpc = pc;
            DPRINTF(RecycledEntry,
                "BRC.update COMMIT pc=%#x BRP++
                (seed: baseline wrong, no WP) cnt=%u lastMBpc=%#x\n",
                pc, (unsigned)stateMachines[h->brpIdx], lastMBpc);
        } else {
            DPRINTF(RecycledEntry,
                "BRC.update COMMIT pc=%#x no WP
                -> baseline correct -> no BRP change\n",
                pc);
        }
    }

    delete h;
    bp_history = nullptr;
}

void
BranchRecyclingCache::squash(ThreadID, void * &bp_history)
{
    if (bp_history) {
        auto *h = static_cast<History*>(bp_history);
        // last mispredicted branch PC for the BRP hash component
        lastMBpc = h->pc;
        delete h;
        bp_history = nullptr;
    }
}

void
BranchRecyclingCache::dumpFinalDebugCounters()
{
    DPRINTF(RecycledEntry,
        "BRC.final recycledCount=%llu, recycledNotTaken=%llu\n",
        (unsigned long long)recycledCount,
        (unsigned long long)recycledNotTaken);
}

} // namespace branch_prediction
} // namespace gem5
