#include "cpu/pred/br_recycling_dyn_inst.hh"

#include "cpu/pred/br_recycling_dyn_inst.hh"

#include <memory>
#include <tuple>

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
      enableTraining(p.enable_training),
      enableStrite(p.enable_strite),
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
            auto &bucket = it->second.entries;
            // auto &stack = std::get<1>(bucket);

            // Only use recycling if has been trained to do so
            if (enableTraining && it->second.trainCounter < 0) {
                return h->base_pred;
            }

            // If strite is enabled, only uses values with a strite bigger than
            // 3
            if (enableStrite && it->second.strideCounter < 3) {
                return h->base_pred;
            }

            if (!bucket.empty()) {
                const Entry &re = bucket.back();
                h->usedRecycle = true;
                h->recycledTaken = re.taken;
                h->brType = re.brType;

                // Pop LIFO
                bucket.pop_back();

                stats.recycledPred++;

                if (h->recycledTaken != h->base_pred) {
                    stats.RecycleBaseDiffer++;
                }

                return h->recycledTaken;
            }
        }
    }

    // Fallback to base predictor.
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

    void *bi = h ? h->tage_bi : nullptr;
    bool made_temp = false;

    if (!bi) {

        base->predict(tid, pc, true, bi);
        made_temp = true;
    }

    base->update(tid, pc, taken, bi, squashed, inst, target);

    // STATS HANDLING: compute predictions and report on squashes
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

        // PC, SRC, BASE_PRED, RECYCLED_PRED, ACTUAL, SQUASHED
        DPRINTF(RecycledEntry, "%#llx;%s;%d;%d;%d;%d\n",
                (unsigned long long)pc, src, base_pred_dbg, recycled_pred,
                (int)taken, (int)squashed);

        return;
    }

    // Update recyclingValue
    // +2 if recycled was correct and base was incorrect
    // +1 if both were correct (As Recycling is quicker than using basePred)
    // -1 if recycled was false/wrong and base was correct
    //  0 otherwise.
    if (h) {
        auto it2 = dynInstStacks.find(pc);

        // if (squashed) {

        //     if (h) {
        //         if (h->base_pred == recycled_pred) {

        //             it2->second.bothWrong++;
        //         } else if (h->usedRecycle && recycled_pred != (int)taken) {

        //             it2->second.incorrectPredictionRecycling++;
        //         } else {

        //             it2->second.incorrectPredictionBase++;
        //         }
        //     }
        // }

        if (it2 != dynInstStacks.end()) {
            auto &recyclingValue = it2->second.trainCounter;
            const bool recycledCorrect =
                (h->usedRecycle && (h->recycledTaken == taken));
            const bool baseCorrect = (h->base_pred == taken);

            if (recycledCorrect && !baseCorrect) {
                recyclingValue += 2;
                it2->second.correctPredictionRecycling++;
            } else if (recycledCorrect && baseCorrect) {
                recyclingValue += 1;
                it2->second.bothCorrect++;
            } else if (!recycledCorrect && baseCorrect) {
                recyclingValue -= 1;
                it2->second.correctPredictionBase++;
            }
        }
    }

    // Cleanup
    if (h) {
        h->tage_bi = bi;
    } else if (made_temp && bi) {
        delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi);
        bi = nullptr;
    }

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
    // Null handling
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
    } else {

        return;
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

// === Internal helpers ===================================================

void
BranchRecyclingCacheDynInst::pushExecutedOutcome(const o3::DynInstPtr &inst)
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

    e.hasOutcome = true;
    auto &bucket = dynInstStacks[pc];

    // Stride counter update
    if (bucket.entries.size() > 0 && bucket.entries.back().taken == e.taken) {
        bucket.strideCounter++;
    } else if (bucket.entries.size() > 0) {
        bucket.strideCounter--;
    }

    bucket.entries.push_back(e);

    stats.committedCount++;
}

void
BranchRecyclingCacheDynInst::notifyExecutedInst(const o3::DynInstPtr &inst)
{
    if (!inst || inst->isSquashed() || !inst->isCondCtrl()) {
        return;
    }
    pushExecutedOutcome(inst);
}

void
BranchRecyclingCacheDynInst::notifySquashedInst(
    const o3::DynInstPtr &mispred_inst, const o3::DynInstPtr &inst)
{
    // DPRINTF(RecycledEntry,
    //         "Notify Squashed inst: PC=%llx, sn=%llu, due to: [PC=%llx,
    //         sn=%i] " "isLoad=%i, " "isCondBranch=%i, isSquashed=%i,
    //         isAddrValid=%i, addr=%llu, \n", inst->pcState().instAddr(),
    //         inst->seqNum, mispred_inst->pcState().instAddr(),
    //         mispred_inst->seqNum, inst->isLoad(), inst->isCondCtrl(),
    //         inst->isSquashed(), inst->effAddrValid(), inst->isLoad() ?
    //         inst->effAddr : 0);

    if (!inst || inst->isSquashed() || !inst->isCondCtrl()) {
        return;
    }

    //   pushCommittedOutcome(inst);
    if (mispred_inst->pcState().instAddr() == inst->pcState().instAddr()) {
        // DPRINTF(RecycledEntry,
        //         "Squashed instance for mispredicted branch: sn=%llu, "
        //         "isExecuted=%i \n",
        //         inst->seqNum, inst->isExecuted());
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
