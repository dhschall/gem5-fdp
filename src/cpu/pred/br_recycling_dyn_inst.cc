#include "cpu/pred/br_recycling_dyn_inst.hh"

#include "cpu/pred/br_recycling_dyn_inst.hh"

#include <memory>
#include <tuple>

#include "base/output.hh"
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

    if (!enableRecycling && !enableStrite) {
        stats.basePred++;
        return h->base_pred;
    }

    // Try LIFO recycled outcome

    auto it = dynInstStacks.find(pc);
    if (it != dynInstStacks.end()) {
        auto &bucket = it->second.entries;

        // Only use recycling if has been trained to do so
        if (enableTraining && it->second.trainCounter < 0) {
            return h->base_pred;
        }

        // Stride-based filtering
        if (enableStrite && it->second.striteCounterDynAddr < 3) {

            return h->base_pred;
        }

        if (!bucket.empty()) {
            const Entry &re = bucket.back();
            h->usedRecycle = true;
            h->recycledTaken = re.taken;
            h->brType = re.brType;

            // DEBUGGING
            if (pc == 4283004) {
                DPRINTF(RecycledEntry,
                        "BRC.lookup pc=%#llx, DynAddrStriteCounter=%i, "
                        "BucketSize=%i, recycledValue=%d, TageValue=%d\n",
                        (unsigned long long)pc,
                        it->second.striteCounterDynAddr, bucket.size(),
                        h->recycledTaken, h->base_pred);
            }

            // DPRINTF(RecycledEntry,
            //         "BRC.lookup pc=%#llx base_pred=%d recycled_pred=%d "
            //         "use from: sn:%llu\n",
            //         (unsigned long long)pc, h->base_pred,
            //         h->recycledTaken, re.seqNum);

            // Pop LIFO
            bucket.pop_back();

            stats.recycledPred++;

            if (h->recycledTaken != h->base_pred) {
                stats.RecycleBaseDiffer++;
            }

            return h->recycledTaken;
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
    // const char *src = (h && h->usedRecycle) ? "recycled" : "base";
    const int recycled_pred =
        (h && h->usedRecycle) ? (int)h->recycledTaken : -1;
    //    const int base_pred_dbg = h ? (int)h->base_pred : -1;

    if (squashed) {
        stats.missPredicts++;

        auto &bs = branchStats[pc];
        bs.mispred++;

        if (h) {
            if (h->base_pred == recycled_pred) {
                stats.missPredictedBothPredictorsWrong++;
            } else if (h->usedRecycle && recycled_pred != (int)taken) {
                stats.missPredictedFaultyRecycle++;

            } else {
                stats.missPredictedTageFault++;
            }
        }

        return;
    }

    auto &bs = branchStats[pc];
    bs.exec++;
    bs.taken += taken ? 1 : 0;

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
    using PairListener =
        ProbeListenerArgFunc<std::pair<o3::DynInstPtr, o3::DynInstPtr>>;
    slistener = cpu->getProbeManager()->connect<PairListener>(
        "SquashInst",
        [this](const std::pair<o3::DynInstPtr, o3::DynInstPtr> p) {
            notifySquashedInst(p.first, p.second);
        });
    // Listener for dependency notifications
    blistener = cpu->getProbeManager()->connect<PairListener>(
        "BranchDependency",
        [this](const std::pair<o3::DynInstPtr, o3::DynInstPtr> p) {
            notifyDependentInst(p.first, p.second);
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

    // DPRINTF(RecycledEntry, "BRC.record pc=%#llx sn:%llu, taken=%i\n",
    //         (unsigned long long)e.pc, e.seqNum, e.taken);

    e.hasOutcome = true;
    auto &bucket = dynInstStacks[pc];

    bucket.entries.push_back(e);
    stats.committedCount++;
}

void
BranchRecyclingCacheDynInst::notifyExecutedInst(const o3::DynInstPtr &inst)
{
    if (!inst || inst->isSquashed() || !inst->isCondCtrl()) {
        return;
    }
    if (enableRecycling) {
        pushExecutedOutcome(inst);
    }
}

void
BranchRecyclingCacheDynInst::notifyDependentInst(
    const o3::DynInstPtr &branch_inst, const o3::DynInstPtr &load_inst)
{

    if (branch_inst->pcState().instAddr() != 4283004) {
        return;
    }

    if (!branch_inst || branch_inst->isSquashed() ||
        !branch_inst->isCondCtrl()) {
        return;
    }

    // Check if notification is duplicate
    if (branch_inst->seqNum == lastSeqNum) {
        return;
    } else {
        lastSeqNum = branch_inst->seqNum;
    }

    const Addr pc = branch_inst->pcState().instAddr();

    Entry e;
    e.pc = pc;
    e.seqNum = branch_inst->seqNum;

    const auto &pcs = branch_inst->pcState();
    e.taken = pcs.branching();

    if (branch_inst->isIndirectCtrl()) {
        e.brType = branch_inst->isUncondCtrl() ? BranchType::IndirectUncond
                                               : BranchType::IndirectCond;
    } else if (branch_inst->isUncondCtrl()) {
        e.brType = BranchType::DirectUncond;
    } else {
        e.brType = BranchType::DirectCond;
    }

    e.hasOutcome = true;
    auto &bucket = dynInstStacks[pc];

    // Stride DynAddr counter update
    int currentDynAddrOffset = load_inst->effAddr - bucket.lastDynAddr;

    if (bucket.lastDynAddrOffset == currentDynAddrOffset) {
        bucket.striteCounterDynAddr++;

        // DPRINTF(RecycledEntry,
        //     " BRC.notifyDependentInst pc=%#llx, "
        //     " DynAddrStride=%i, StrideCounterDynAddr=%i\n",
        //     (unsigned long long)e.pc,
        //     currentDynAddrOffset, bucket.striteCounterDynAddr);
    } else if (bucket.striteCounterDynAddr > 0) {
        bucket.striteCounterDynAddr--;
        return;
    }

    DPRINTF(RecycledEntry,
            "Notify dependent load inst: Branch [PC=%llx, sn=%llu, Taken = "
            "%i, CurrentStrite = %i] -> load "
            "[PC=%llx, sn=%i, Addr=%llu, Taken=%i] \n",
            branch_inst->pcState().instAddr(), branch_inst->seqNum,
            branch_inst->pcState().branching(), bucket.striteCounterDynAddr,
            load_inst->pcState().instAddr(), load_inst->seqNum,
            load_inst->effAddr, load_inst->pcState().branching());

    bucket.lastDynAddr = load_inst->effAddr;
    bucket.lastDynAddrOffset = currentDynAddrOffset;

    bucket.entries.push_back(e);
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

void
BranchRecyclingCacheDynInst::dump(const std::string &filename)
{
    // DPRINTF(RecycledEntry, "Dump branch statistics to %s\n", filename);

    std::ofstream fileStream(simout.resolve(filename), std::ios::out);
    if (!fileStream.good()) {
        panic("Could not open %s for writing\n", filename);
    }

    ccprintf(fileStream, "pc;exec;taken;mispred\n");

    // TAGE tables
    for (auto &bi : branchStats) {
        ccprintf(fileStream, "%llu;%i;%i;%i\n", bi.first, bi.second.exec,
                 bi.second.taken, bi.second.mispred);
    }
    fileStream.close();
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
