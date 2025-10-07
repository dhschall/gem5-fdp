#include "cpu/pred/br_recycling_dyn_inst.hh"

#include <deque>
#include <memory>
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/branch_type.hh"
#include "debug/Fetch.hh"
#include "debug/RecycledEntry.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace branch_prediction
{

static inline bool
isIndirect(BranchType t)
{
    return t == BranchType::IndirectUncond || t == BranchType::IndirectCond ||
           t == BranchType::CallIndirect;
}

BranchRecyclingCacheDynInst::BranchRecyclingCacheDynInst(const Params &p)
    : ConditionalPredictor(p),
      base(p.base),
      enableRecycling(p.enable_recycling),
      // BRP params as paper recomend
      mBits(14),
      nShift(7), //=m/2
      brpSize(16384),
      stateMachines(brpSize, SatCounter8(2)),
      ghr(p.numThreads, 0),
      lastMBpc(0),
      dynInstCache(),
      cpu(nullptr),
      stats(this)
{
    // First initialize the base predictor
    base->tage->init();

    // Next the recycling cache
    for (auto &c : stateMachines) { ++c; }

    DPRINTF(RecycledEntry, "BRC-DynInst: constructed (BRP=%u, m=%u, n=%u)\n",
            brpSize, mBits, nShift);
}

// Index only by PC?
inline unsigned
BranchRecyclingCacheDynInst::stateMachineIdx(ThreadID tid, Addr cb_pc) const
{
    const uint64_t mask = (mBits >= 31) ? 0xffffffffu : ((1u << mBits) - 1u);
    const uint64_t bhr = static_cast<uint64_t>(ghr[tid]) & mask;
    const uint64_t cb = static_cast<uint64_t>(cb_pc >> 2) & mask;
    const uint64_t mb =
        static_cast<uint64_t>((lastMBpc >> 2) << nShift) & mask;

    const uint64_t x = bhr ^ cb ^ mb;
    return (x & (brpSize - 1));
}

inline bool
BranchRecyclingCacheDynInst::brpSaysUse(ThreadID tid, Addr cb_pc) const
{
    return (stateMachines[stateMachineIdx(tid, cb_pc)] > 1);
}

inline void
BranchRecyclingCacheDynInst::brpTrainByIdx(unsigned idx, bool good)
{
    auto &c = stateMachines[idx];
    if (good) {
        ++c;
    } else {
        --c;
    }
}

bool
BranchRecyclingCacheDynInst::lookup(ThreadID tid, Addr pc, void *&bp_history)
{
    bp_history = nullptr;

    // First base prediction
    auto *h = new History();

    h->base_pred = base->predict(tid, pc, true, h->tage_bi);
    bp_history = h;

    const unsigned idx_now = stateMachineIdx(tid, pc);

    auto it = dynInstCache.find(pc);
    auto dynIt = dynInstCache.find(pc);
    /*  DPRINTF(RecycledEntry, "BRC.lookup pc=%#x, brpSaysUse=%d\n",
             pc, (int)brpSaysUse(tid, pc)); */

    // Try to use a recycled outcome from BRC
    if (it != dynInstCache.end() && !it->second.empty() &&
        brpSaysUse(tid, pc)) {

        // DPRINTF(RecycledEntry, "BRC.lookup pc=%#x found qsize=%#x\n", pc,
        //         it->second.size());
        auto &q = it->second;
        const Entry &re = q.front();

        h->pc = pc;
        h->usedRecycle = true;
        h->recycledTaken = re.taken;
        h->actualKnown = false;
        h->brType = re.brType;
        h->brpIdx = idx_now;

        q.pop_front();

        // DPRINTF(RecycledEntry,
        //         "BRC.lookup pc=%#x recycled dir=%d (q=%zu) idx=%u\n", pc,
        //         (int)re.taken, q.size(), idx_now);

    } else if (dynIt != dynInstCache.end() && !dynIt->second.empty()) {
    //&&           brpSaysUse(tid, pc)) {

        // DPRINTF(RecycledEntry, "BRC.lookup pc=%#x found qsize=%#x\n", pc,
        //         dynIt->second.size());
        auto &q = dynIt->second;
        const Entry &re = q.front();

        h->pc = pc;
        h->usedRecycle = true;
        h->recycledTaken = re.taken;
        h->actualKnown = false;
        h->brType = re.brType;
        h->brpIdx = idx_now;

        q.pop_front();

    } else {

        h->pc = pc;
        h->usedRecycle = false;
        h->recycledTaken = false;
        h->actualKnown = false;
        h->brType = BranchType::DirectCond;
        h->brpIdx = idx_now;
        bp_history = h;

        /* DPRINTF(RecycledEntry,
        "BRC.lookup pc=%#x fallback dir=0 idx=%u\n", pc, idx_now);
    */
    }

    if (!enableRecycling) {
        stats.basePred++;
        return h->base_pred;
    }
    if (h->usedRecycle) {
        // DPRINTF(RecycledEntry, "BRC.lookup pc=%#x used recycled pred=%d\n",
        // pc,
        //         (int)h->recycledTaken);
        stats.recycledCount++;
        return h->recycledTaken;
    } else {
        // DPRINTF(RecycledEntry, "BRC.lookup pc=%#x used base pred=%d\n", pc,
        //         (int)h->base_pred);
        stats.basePred++;
        return h->base_pred;
    }
}

void
BranchRecyclingCacheDynInst::branchPlaceholder(ThreadID tid, Addr pc,
                                               bool uncond, void *&bpHistory)
{
    assert(!bpHistory);

    auto *h = new History();
    base->branchPlaceholder(tid, pc, uncond, h->tage_bi);

    h->pc = pc;
    h->usedRecycle = false;
    h->actualKnown = false;
    h->brType = uncond ? BranchType::DirectUncond : BranchType::DirectCond;
    h->brpIdx = stateMachineIdx(tid, pc);
    bpHistory = h;
    /*
        DPRINTF(RecycledEntry, "BRC.placeholder
        pc=%#x idx=%u\n", pc, h->brpIdx); */
}

void
BranchRecyclingCacheDynInst::updateHistories(ThreadID tid, Addr pc,
                                             bool uncond, bool taken,
                                             Addr target,
                                             const StaticInstPtr &inst,
                                             void *&bp_history)
{
    History *h;
    if (bp_history == nullptr) {
        assert(uncond);
        h = new History();
        h->pc = pc;
        h->usedRecycle = false;
        h->actualKnown = false;
        h->brpIdx = stateMachineIdx(tid, pc);
        bp_history = h;
    } else {
        h = static_cast<History *>(bp_history);
    }

    // h->actualKnown = true;
    // h->actualTaken = taken;

    // if (inst) {
    //     if (inst->isIndirectCtrl()) {
    //         h->brType =
    //             uncond ? BranchType::IndirectUncond : BranchType::IndirectCond;
    //     } else if (inst->isUncondCtrl()) {
    //         h->brType = BranchType::DirectUncond;
    //     } else {
    //         h->brType = BranchType::DirectCond;
    //     }
    //}

    // const unsigned mask = (mBits >= 31) ? 0xffffffffu : ((1u << mBits) - 1u);
    // ghr[tid] = ((ghr[tid] << 1) | (taken ? 1u : 0u)) & mask;
    // /*
    //     DPRINTF(RecycledEntry, "BRC.updateHistories pc=%#x
    //     taken=%d ghr=%#x\n", pc, (int)taken, (unsigned)ghr[tid]);
    //  */
    base->updateHistories(tid, pc, uncond, taken, target, inst, h->tage_bi);
}

void
BranchRecyclingCacheDynInst::update(ThreadID tid, Addr pc, bool taken,
                                    void *&bp_history, bool squashed,
                                    const StaticInstPtr &inst, Addr target)
{
    assert(bp_history);
    auto *h = static_cast<History *>(bp_history);
    TAGE_SC_L::TageSCLBranchInfo *tage_bi =
        static_cast<TAGE_SC_L::TageSCLBranchInfo *>(h->tage_bi);

    // h->actualKnown = true;
    // h->actualTaken = taken;

    // if (inst) {
    //     if (inst->isIndirectCtrl()) {
    //         h->brType = inst->isUncondCtrl() ? BranchType::IndirectUncond
    //                                          : BranchType::IndirectCond;
    //     } else if (inst->isUncondCtrl()) {
    //         h->brType = BranchType::DirectUncond;
    //     } else {
    //         h->brType = BranchType::DirectCond;
    //     }
    // }

    if (squashed) {

    //     Entry e;
    //     e.pc = pc;
    //     e.hasOutcome = true;
    //     e.taken = taken;
    //     e.brType = h->brType;

    //     auto &q = dynInstCache[pc];
    //     q.emplace_back(std::move(e));

    //     lastMBpc = pc;
    //     /*
    //             DPRINTF(RecycledEntry, "BRC.update SQUASH pc=%#x
    //             queued wrong-path dir=%d (q=%zu); lastMBpc=%#x\n",
    //              pc, (int)taken, q.size(), lastMBpc);
    //      */
        base->update(tid, pc, taken, tage_bi, squashed, inst, target);
        return;
    }

    // Do the base predictor update.
    base->update(tid, pc, taken, tage_bi, squashed, inst, target);

    // // const bool baselineCorrect = !h->actualTaken;
    // const bool baselineCorrect = (h->base_pred == h->actualTaken);
    // const bool wpEqCp = h->usedRecycle && (h->recycledTaken == h->actualTaken);

    // // Train how described in Paper
    // // if (h->usedRecycle) {
    // if (!baselineCorrect && wpEqCp) {
    //     // If baseline mispredicted && WP==CP -> increment
    //     brpTrainByIdx(h->brpIdx, true);
    //     lastMBpc = pc;
    //     stats.training0++;

    // } else if (baselineCorrect && !wpEqCp) {
    //     // If baseline correct && WP!=CP -> decrement
    //     brpTrainByIdx(h->brpIdx, false);
    //     stats.training1++;
    // } else if (!baselineCorrect) {
    //     // Also train if WP wasn't used contrary to
    //     // paper otherwise training would never start
    //     brpTrainByIdx(h->brpIdx, true);
    //     lastMBpc = pc;
    //     stats.training2++;
    // }

    delete tage_bi;
    delete h;
    bp_history = nullptr;
}

void
BranchRecyclingCacheDynInst::squash(ThreadID tid, void *&bp_history)
{
    assert(bp_history);
    auto *h = static_cast<History *>(bp_history);

    // last mispredicted branch PC for the BRP hash component
    lastMBpc = h->pc;

    base->squash(tid, h->tage_bi);
    delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(h->tage_bi);
    h->tage_bi = nullptr;
    delete h;
    bp_history = nullptr;
}

BranchRecyclingCacheDynInst::BranchRecyclingCacheDynInstStats::
    BranchRecyclingCacheDynInstStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(recycledCount, statistics::units::Count::get(),
               "Number of entries recycled"),
      ADD_STAT(recycledNotTaken, statistics::units::Count::get(),
               "Number of times a non recycled path was taken"),
      ADD_STAT(basePred, statistics::units::Count::get(),
               "Number of times the base predictor was used"),
      ADD_STAT(
          training0, statistics::units::Count::get(),
          "Number of times BRP was trained: baseline mispredicted && WP==CP"),
      ADD_STAT(training1, statistics::units::Count::get(),
               "Number of times BRP was trained: baseline correct && WP!=CP"),
      ADD_STAT(training2, statistics::units::Count::get(),
               "Number of times BRP was trained: baseline mispredicted"),
      ADD_STAT(training3, statistics::units::Count::get(),
               "Number of times BRP was consulted"),
      ADD_STAT(faultyRecycle, statistics::units::Count::get(),
               "Number of times a recycled entry was used when it shouldn't "
               "have been")
{}

void
BranchRecyclingCacheDynInst::regProbeListeners()
{
    ConditionalPredictor::regProbeListeners();

    if (cpu == nullptr) {
        warn(
            "BranchRecyclingCacheDynInst: No CPU to listen from registered\n");
        return;
    }
    typedef ProbeListenerArgFunc<o3::DynInstPtr> InstListener;
    listener = cpu->getProbeManager()->connect<InstListener>(
        "ToCommit",
        [this](const o3::DynInstPtr &inst) { notifyExecutedInst(inst);
    });
    
    typedef ProbeListenerArgFunc<std::pair<o3::DynInstPtr, o3::DynInstPtr>> SquashListener;
    slistener = cpu->getProbeManager()->connect<SquashListener>(
        "SquashInst",
        [this](const std::pair<o3::DynInstPtr, o3::DynInstPtr> p) {
             notifySquashedInst(p.first, p.second); 
    });
}

void
BranchRecyclingCacheDynInst::notifyExecutedInst(const o3::DynInstPtr &inst)
{

    DPRINTF(RecycledEntry,
            "Notify: PC=%llx, sn=%llu, isLoad=%i, "
            "isCondBranch=%i, isSquashed=%i, isAddrValid=%i, addr=%llu, \n",
            inst->pcState().instAddr(), inst->seqNum, inst->isLoad(),
            inst->isCondCtrl(), inst->isSquashed(), inst->effAddrValid(),
            inst->isLoad() ? inst->effAddr : 0);

    if (!inst || inst->isSquashed() || !inst->isCondCtrl()) {
        return;
    }

    Entry e;
    e.pc = inst->pcState().instAddr();
    e.hasOutcome = true;

    const auto &pc = inst->pcState();
    e.taken = pc.branching();

    if (inst->isIndirectCtrl()) {
        e.brType = inst->isUncondCtrl() ? BranchType::IndirectUncond
                                        : BranchType::IndirectCond;
    } else if (inst->isUncondCtrl()) {
        e.brType = BranchType::DirectUncond;
    } else {
        e.brType = BranchType::DirectCond;
    }

    auto &q = dynInstCache[e.pc];
    q.emplace_back(e);

    DPRINTF(RecycledEntry,
            "BRC.notify pc=%#x seq=%llu committed dir=%d (q=%zu)\n", e.pc,
            inst->seqNum, static_cast<int>(e.taken), q.size());
}

void
BranchRecyclingCacheDynInst::notifySquashedInst(const o3::DynInstPtr &mispred_inst, const o3::DynInstPtr &inst)
{
    DPRINTF(RecycledEntry,
            "Notify Squashed inst: PC=%llx, sn=%llu, due to: [PC=%llx, sn=%i] isLoad=%i, "
            "isCondBranch=%i, isSquashed=%i, isAddrValid=%i, addr=%llu, \n",
            inst->pcState().instAddr(), inst->seqNum,
            mispred_inst->pcState().instAddr(), mispred_inst->seqNum,
            inst->isLoad(),
            inst->isCondCtrl(), inst->isSquashed(), inst->effAddrValid(),
            inst->isLoad() ? inst->effAddr : 0);

    if (!inst || inst->isSquashed() || !inst->isCondCtrl()) {
        return;
    }

    if (mispred_inst->pcState().instAddr() == inst->pcState().instAddr()) {
        DPRINTF(RecycledEntry,
            "Squashed instance for mispredicted branch: sn=%llu, isExecuted=%i \n",
            inst->seqNum, inst->isExecuted());
    }
}



} // namespace branch_prediction
} // namespace gem5

// Sort Cache by Sequence number and not first finished
