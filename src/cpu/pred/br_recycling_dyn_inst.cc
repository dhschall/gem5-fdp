#include "cpu/pred/br_recycling_dyn_inst.hh"

#include <fstream>
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
      enableTraining2(p.enable_training2),
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

    if (!enableRecycling && !enableStrite && !enableTraining2) {
        stats.basePred++;
        return h->base_pred;
    }

    // Try LIFO recycled outcome
    auto it = dynInstStacks.find(pc);
    if (it != dynInstStacks.end()) {
        auto &entries = it->second.entries;

        // Only use recycling if has been trained to do so
        if (enableTraining && it->second.trainCounter < 0) {
            if (!entries.empty()) {
                entries.pop_back();
            }
            return h->base_pred;
        }

        // Recycling2: only if trained to use recycling
        if (enableTraining2 && it->second.useRecycle == false) {
            if (!entries.empty()) {
                entries.pop_back();
            }
            return h->base_pred;
        }

        // Stride-based filtering
        if (enableStrite && it->second.striteCounterDynAddr < 50) {
            if (!entries.empty()) {
                entries.pop_back();
            }
            return h->base_pred;
        }

        if (!entries.empty()) {
            const Entry &re = entries.back();
            h->usedRecycle = true;
            h->recycledTaken = re.taken;
            h->brType = re.brType;

            // Pop LIFO
            entries.pop_back();

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

    // Make sure we have base predictor info to update it
    void *bi = h ? h->tage_bi : nullptr;
    bool made_temp = false;

    if (!bi) {
        base->predict(tid, pc, true, bi);
        made_temp = true;
    }

    base->update(tid, pc, taken, bi, squashed, inst, target);

    // Per-PC stats (always count execution)
    auto &bs = branchStats[pc];
    bs.exec++;
    bs.taken += taken ? 1 : 0;

    // Bucket (only if it exists)
    auto it_bucket = dynInstStacks.find(pc);
    if (it_bucket != dynInstStacks.end()) {
        auto &bucket = it_bucket->second;
        bucket.execs++;

        // Training / correctness bookkeeping (only if we have history)
        if (h) {
            const bool recycledCorrect =
                (h->usedRecycle && (h->recycledTaken == taken));
            const bool baseCorrect = (h->base_pred == taken);

            if (recycledCorrect && !baseCorrect) {
                bucket.trainCounter += 2;
                bucket.correctPredictionRecycling++;
            } else if (recycledCorrect && baseCorrect) {
                bucket.trainCounter += 1;
                bucket.bothCorrect++;
            } else if (!recycledCorrect && baseCorrect) {
                bucket.trainCounter -= 1;
                bucket.correctPredictionBase++;
            }
        }

        // Mispredict handling
        if (squashed) {
            bs.mispred++;
            stats.missPredicts++;

            if (h) {
                const int recycled_pred =
                    (h->usedRecycle) ? (int)h->recycledTaken : -1;

                if (h->usedRecycle && h->base_pred == recycled_pred) {
                    stats.missPredictedBothPredictorsWrong++;
                    bucket.mispredictsTage++;
                    bucket.mispredictsRecycle++;
                } else if (h->usedRecycle && recycled_pred != (int)taken) {
                    stats.missPredictedFaultyRecycle++;
                    bucket.mispredictsRecycle++;
                } else {
                    stats.missPredictedTageFault++;
                    bucket.mispredictsTage++;
                }

                // Periodically decide whether to use recycling
                if (bucket.execs % 100 == 0) {
                    if (bucket.mispredictsRecycle < bucket.mispredictsTage) {
                        bucket.useRecycle = true;
                    } else if (bucket.mispredictsRecycle >
                               bucket.mispredictsTage) {
                        bucket.useRecycle = false;
                    }
                    bucket.mispredictsTage = 0;
                    bucket.mispredictsRecycle = 0;
                }
            } else {
                // No history => attribute to base
                stats.missPredictedTageFault++;
                bucket.mispredictsTage++;
            }
        }
    } else {
        // No bucket exists; still count global mispredict stats
        if (squashed) {
            bs.mispred++;
            stats.missPredicts++;
        }
    }

    // Cleanup predictor info objects
    if (h) {
        // keep it attached for deletion below
        h->tage_bi = bi;
    } else if (made_temp && bi) {
        delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi);
        bi = nullptr;
    }

    // Free history (gem5 predictors typically allocate per-branch history)
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
    History *h = bp_history ? static_cast<History *>(bp_history) : nullptr;
    if (!h) {
        return;
    }

    void *bi = h->tage_bi;
    if (bi) {
        base->squash(tid, bi);
        delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi);
        bi = nullptr;
    }

    h->tage_bi = nullptr;

    delete h;
    bp_history = nullptr;
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
    if (!branch_inst || branch_inst->isSquashed() ||
        !branch_inst->isCondCtrl()) {
        return;
    }

    // Check if notification is duplicate
    if (branch_inst->seqNum == lastSeqNum) {
        return;
    }
    lastSeqNum = branch_inst->seqNum;

    const Addr pc = branch_inst->pcState().instAddr();
    auto &bucket = dynInstStacks[pc];

    // Stride DynAddr counter update
    int currentDynAddrOffset = load_inst->effAddr - bucket.lastDynAddr;

    if (bucket.lastDynAddrOffset == currentDynAddrOffset) {
        bucket.striteCounterDynAddr++;

        if (bucket.striteCounterDynAddr > branchStats[pc].biggestStrite) {
            branchStats[pc].biggestStrite = bucket.striteCounterDynAddr;
        }
    } else if (bucket.striteCounterDynAddr > 0) {
        bucket.striteCounterDynAddr = 0;
        return;
    }

    DPRINTF(
        RecycledEntry,
        "Notify dependent load inst: Branch [PC=%llx, sn=%llu, Taken=%i, "
        "CurrentStrite=%i] -> load [PC=%llx, sn=%llu, Addr=%llu, Taken=%i]\n",
        (unsigned long long)branch_inst->pcState().instAddr(),
        (unsigned long long)branch_inst->seqNum,
        (int)branch_inst->pcState().branching(),
        (int)bucket.striteCounterDynAddr,
        (unsigned long long)load_inst->pcState().instAddr(),
        (unsigned long long)load_inst->seqNum,
        (unsigned long long)load_inst->effAddr,
        (int)load_inst->pcState().branching());

    bucket.lastDynAddr = load_inst->effAddr;
    bucket.lastDynAddrOffset = currentDynAddrOffset;
}

void
BranchRecyclingCacheDynInst::notifySquashedInst(
    const o3::DynInstPtr &mispred_inst, const o3::DynInstPtr &inst)
{
    if (!inst || inst->isSquashed() || !inst->isCondCtrl()) {
        return;
    }

    if (mispred_inst &&
        mispred_inst->pcState().instAddr() == inst->pcState().instAddr()) {
        // debug if needed
    }
}

void
BranchRecyclingCacheDynInst::dump(const std::string &filename)
{
    std::ofstream fileStream(simout.resolve(filename), std::ios::out);
    if (!fileStream.good()) {
        panic("Could not open %s for writing\n", filename);
    }

    ccprintf(fileStream, "pc;exec;taken;mispred\n");

    for (auto &bi : branchStats) {
        ccprintf(fileStream, "%llu;%i;%i;%i;%i\n",
                 (unsigned long long)bi.first, bi.second.exec, bi.second.taken,
                 bi.second.mispred, bi.second.biggestStrite);
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
