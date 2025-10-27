#include "cpu/pred/br_recycling_dyn_inst.hh"

#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/branch_type.hh"
#include "debug/RecycledEntry.hh"
namespace gem5
{
namespace branch_prediction
{

BranchRecyclingCacheDynInst::BranchRecyclingCacheDynInst(const Params &p)
    : ConditionalPredictor(p),
      base(p.base),
      enableRecycling(p.enable_recycling),
      dynInstStacks(),
      seqToPc(),
      cpu(nullptr),
      stats(this)
{
    base->init();
}

bool
BranchRecyclingCacheDynInst::lookup(ThreadID tid, Addr pc, void *&bp_history)
{
    bp_history = nullptr;

    // Make Base prediction
    auto *h = new History();
    h->base_pred = base->predict(tid, pc, true, h->tage_bi);
    bp_history = h;

    // Set history fields
    h->pc = pc;
    h->usedRecycle = false;
    h->recycledTaken = false;
    h->actualKnown = false;
    h->brType = BranchType::DirectCond;

    // Try LIFO recycled outcome
    if (enableRecycling) {

        auto it = dynInstStacks.find(pc);
        if (it != dynInstStacks.end()) {
            auto &stack = it->second;

            //    if (!stack.empty()) {
            if (!stack.empty()) {

                const Entry &re = stack.back();
                h->usedRecycle = true;
                h->recycledTaken = re.taken;
                h->brType = re.brType;

                // Pop LIFO
                // DPRINTF(RecycledEntry,
                //     "BRC.lookup pc=%#llx recycled dir=%d sn=%llu
                //     (remain=%llu)\n", (unsigned long long)pc, (int)re.taken,
                //     (unsigned long long)re.seqNum,
                //     (unsigned long long)(stack.size() - 1));

                stack.pop_back();
                stats.recycledPred++;

                if (h->recycledTaken != h->base_pred) {
                    stats.RecycleBaseDiffer++;
                }

                return h->recycledTaken;
            }
        }
    }

    // Fallback to base predictor
    stats.basePred++;
    return h->base_pred;
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
    bpHistory = h;

    // DPRINTF(RecycledEntry, "BRC.placeholder pc=%#llx idx=%u\n",
    //         (unsigned long long)pc, h->brpIdx);
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

        bp_history = h;
    } else {
        h = static_cast<History *>(bp_history);
    }

    base->updateHistories(tid, pc, uncond, taken, target, inst, h->tage_bi);
}

void
BranchRecyclingCacheDynInst::update(ThreadID tid, Addr pc, bool taken,
                                    void *&bp_history, bool squashed,
                                    const StaticInstPtr &inst, Addr target)
{
    History *h = bp_history ? static_cast<History *>(bp_history) : nullptr;

    // Record actual outcome
    if (h) {
        h->actualKnown = true;
        h->actualTaken = taken;
    }

    // Ensure we have a BI for base predictor
    void *bi = h ? h->tage_bi : nullptr;
    bool made_temp = false;
    if (!bi) {
        base->predict(tid, pc, true, bi);
        made_temp = true;
    }

    // Forward to base predictor
    base->update(tid, pc, taken, bi, squashed, inst, target);

    // STATS HANDLING

    const char *src = (h && h->usedRecycle) ? "recycled" : "base";
    const int recycled_pred =
        (h && h->usedRecycle) ? (int)h->recycledTaken : -1;
    const int base_pred_dbg = h ? (int)h->base_pred : -1;

    if (squashed) {
        stats.missPredicts++;
        if (h) {
            if (h->base_pred == recycled_pred) {
                stats.missPredictedBothPredictorsWrong++;
            } else if (h->usedRecycle && recycled_pred != (int)taken) {
                stats.missPredictedFaultyRecycle++;
            } else {
                stats.missPredictedTageFault++;
            }
        }
    }

    DPRINTF(RecycledEntry,
            "BRC.update pc=%#llx src=%s base_pred=%d recycled_pred=%d "
            "actual=%d squashed=%d\n",
            (unsigned long long)pc, src, base_pred_dbg, recycled_pred,
            (int)taken, (int)squashed);

    // Preserve BI ownership or free temp BI
    if (h) {
        h->tage_bi = bi;
    } else if (made_temp && bi) {
        delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi);
        bi = nullptr;
    }

    // Cleanup history if present
    if (h) {
        if (h->tage_bi) {
            delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(h->tage_bi);
            h->tage_bi = nullptr;
        }
        delete h;
        bp_history = nullptr;
    }
}

void
BranchRecyclingCacheDynInst::squash(ThreadID tid, void *&bp_history)
{
    // null check
    History *h = bp_history ? static_cast<History *>(bp_history) : nullptr;

    if (h) {

        void *bi = h->tage_bi;
        if (bi) {
            base->squash(tid, bi);
        }
        if (bi) {
            delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi);
            bi = nullptr;
        }
        h->tage_bi = nullptr;

        delete h;
        bp_history = nullptr;
    }
}

void
BranchRecyclingCacheDynInst::regProbeListeners()
{
    ConditionalPredictor::regProbeListeners();

    if (cpu == nullptr) {
        warn("BranchRecyclingCacheDynInst: No CPU registered; probes not "
             "connected\n");
        return;
    }

    // Listener for instructions reaching ToCommit
    using InstListener = ProbeListenerArgFunc<o3::DynInstPtr>;
    listener = cpu->getProbeManager()->connect<InstListener>(
        "ToCommit",
        [this](const o3::DynInstPtr &inst) { notifyExecutedInst(inst); });

    // Listener for squash notifications (mispred anchor + squashed inst)
    using SquashListener =
        ProbeListenerArgFunc<std::pair<o3::DynInstPtr, o3::DynInstPtr>>;
    slistener = cpu->getProbeManager()->connect<SquashListener>(
        "SquashInst",
        [this](const std::pair<o3::DynInstPtr, o3::DynInstPtr> p) {
            notifySquashedInst(p.first, p.second);
        });
}

// Internal Helpers

void
BranchRecyclingCacheDynInst::pushCommittedOutcome(const o3::DynInstPtr &inst)
{
    if (!inst) {
        return;
    }

    const Addr pc = inst->pcState().instAddr();

    Entry e;
    e.pc = pc;
    e.seqNum = inst->seqNum;

    const auto &pcs = inst->pcState();
    e.taken = pcs.branching();

    if (inst->isIndirectCtrl()) {
        e.brType = inst->isUncondCtrl() ? BranchType::IndirectUncond
                                        : BranchType::IndirectCond;
    } else if (inst->isUncondCtrl()) {
        e.brType = BranchType::DirectUncond;
    } else {
        e.brType = BranchType::DirectCond;
    }

    auto &vec = dynInstStacks[pc];
    vec.push_back(e);
    stats.committedCount++;

    // DPRINTF(RecycledEntry,
    //     "BRC.push pc=%#llx sn=%llu dir=%d (stack=%llu)\n",
    //     (unsigned long long)pc,
    //     (unsigned long long)e.seqNum,
    //     (int)e.taken,
    //     (unsigned long long)vec.size());
}

void
BranchRecyclingCacheDynInst::notifyExecutedInst(const o3::DynInstPtr &inst)
{
    // check if inst is valid and record
    if (inst) {
        // DPRINTF(RecycledEntry,
        //         "Notify(ToCommit): PC=%#llx, sn=%llu, isLoad=%i, "
        //         "isCondBranch=%i, isSquashed=%i, isAddrValid=%i,
        //         addr=%llu\n", (unsigned long
        //         long)inst->pcState().instAddr(), (unsigned long
        //         long)inst->seqNum, inst->isLoad(), inst->isCondCtrl(),
        //         inst->isSquashed(),
        //         inst->effAddrValid(),
        //         inst->isLoad() ? (unsigned long long)inst->effAddr : 0ULL);
    }

    // only recycle valid, executed, non squashed and conditional branches

    if (!inst || !inst->isCondCtrl() || !inst->isExecuted())

    {
        return;
    }

    // Push committed outcome for recycling
    pushCommittedOutcome(inst);
}

void
BranchRecyclingCacheDynInst::notifySquashedInst(
    const o3::DynInstPtr &mispred_inst, const o3::DynInstPtr &inst)
{
    // const Addr mp_pc = mispred_inst ? mispred_inst->pcState().instAddr() :
    // 0; DPRINTF(RecycledEntry,
    //         "NotifySquash: PC=%#llx (sn=%llu) due to [PC=%#llx sn=%llu], "
    //         "isCond=%i, wasExec=%i\n",
    //         (unsigned long long)(inst ? inst->pcState().instAddr() : 0ULL),
    //         (unsigned long long)(inst ? inst->seqNum : 0ULL),
    //         (unsigned long long)mp_pc,
    //         (unsigned long long)(mispred_inst ? mispred_inst->seqNum :
    //         0ULL), (int)(inst && inst->isCondCtrl()), (int)(inst &&
    //         inst->isExecuted()));

    if (!inst) {
        return;
    }

    // If the squashed inst is itself the mispredicted branch (same static PC),
    // record lastMBpc for BRP hashing.
    if (mispred_inst &&
        mispred_inst->pcState().instAddr() == inst->pcState().instAddr()) {
        // DPRINTF(RecycledEntry,
        //         "BRC.squash anchor pc=%#llx sn=%llu exec=%i\n",
        //         (unsigned long long)lastMBpc,
        //         (unsigned long long)inst->seqNum,
        //         (int)inst->isExecuted());
    }
}

BranchRecyclingCacheDynInst::BranchRecyclingCacheDynInstStats::
    BranchRecyclingCacheDynInstStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(recycledPred, statistics::units::Count::get(),
               "Number of entries recycled"),
      ADD_STAT(basePred, statistics::units::Count::get(),
               "Number of times the base predictor was used"),
      ADD_STAT(RecycleBaseDiffer, statistics::units::Count::get(),
               "Number of times only the recycled prediction was correct"),
      ADD_STAT(missPredictedFaultyRecycle, statistics::units::Count::get(),
               "Number of times a faulty recycle caused a misprediction"),
      ADD_STAT(
          missPredictedTageFault, statistics::units::Count::get(),
          "Number of times a TAGE predictor fault caused a misprediction"),
      ADD_STAT(missPredictedBothPredictorsWrong,
               statistics::units::Count::get(),
               "Number of times both predictors were wrong causing a "
               "misprediction"),
      ADD_STAT(missPredicts, statistics::units::Count::get(),
               "Number of times a misprediction occurred"),
      ADD_STAT(committedCount, statistics::units::Count::get(),
               "Number of committed branches to dynInstStack")
{}

} // namespace branch_prediction
} // namespace gem5
