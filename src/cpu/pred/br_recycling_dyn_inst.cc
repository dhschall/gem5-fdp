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

BranchRecyclingCacheDynInst::BucketNode *
BranchRecyclingCacheDynInst::BucketCache::find(Addr pc)
{
    auto it = BRC.find(pc);
    if (it == BRC.end()) {
        return nullptr;
    }
    touch(it->second);
    return &it->second;
}

void
BranchRecyclingCacheDynInst::BucketCache::evictBucket()
{
    if (BRC.empty()) {
        return;
    }

    // LFU mechanism use oldest as tie breaker
    Addr evict_bucket_key = 0;
    bool evict_found = false;

    uint64_t best_freq = std::numeric_limits<uint64_t>::max();
    uint64_t best_stamp = std::numeric_limits<uint64_t>::max();

    for (auto &bucket : BRC) {
        const Addr key = bucket.first;
        const BucketNode &n = bucket.second;

        if (!evict_found || (n.freq < best_freq) ||
            (n.freq == best_freq && n.stamp < best_stamp)) {
            evict_found = true;
            evict_bucket_key = key;
            best_freq = n.freq;
            best_stamp = n.stamp;
        }
    }

    BRC.erase(evict_bucket_key);
}

BranchRecyclingCacheDynInst::BucketNode *
BranchRecyclingCacheDynInst::BucketCache::getOrAllocBucket(Addr pc)
{
    // return found entry
    if (auto *n = find(pc)) {
        return n;
    }

    // If full, evict
    if (BRC.size() >= maxBuckets) {
        evictBucket();
    }

    // Insert new node
    auto [it, inserted] = BRC.emplace(pc, BucketNode{});
    auto &node = it->second;
    node.bucket.reset();
    node.freq = 0;
    node.stamp = 0;

    touch(node);
    return &node;
}

BranchRecyclingCacheDynInst::BranchRecyclingCacheDynInst(const Params &p)
    : ConditionalPredictor(p),
      base(p.base),
      enableRecycling(p.enable_recycling),
      enableTraining(p.enable_training),
      enableStride(p.enable_stride),
      bucketCache(p.bucket_count),
      bucketSize(p.bucket_size),
      trainingInterval(p.training_interval),
      strideConfidenceThreshold(p.stride_confidence_threshold),
      cpu(nullptr),
      stats(this)
{
    base->init();
}

BranchRecyclingCacheDynInst::RecyclingBucket *
BranchRecyclingCacheDynInst::findBucket(Addr pc)
{
    auto *n = bucketCache.find(pc);
    return n ? &n->bucket : nullptr;
}

BranchRecyclingCacheDynInst::RecyclingBucket *
BranchRecyclingCacheDynInst::getOrAllocBucket(Addr pc)
{
    auto *n = bucketCache.getOrAllocBucket(pc);
    return &n->bucket;
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

    if (!enableRecycling && !enableStride && !enableTraining) {
        stats.basePred++;
        return h->base_pred;
    }

    // check if entry exists
    RecyclingBucket *bucket = findBucket(pc);
    if (bucket) {

        // recycling mechanism
        if (enableTraining && bucket->useRecycle == false) {
            Entry tmp;
            (void)bucket->popFifo(tmp); // drop oldest to keep aligned
            DPRINTF(RecycledEntry, "Used base prediction for pc %#x\n", pc);
            return h->base_pred;
        }

        // stride detection
        if (enableStride &&
            bucket->strideCounterDynAddr < strideConfidenceThreshold) {
            Entry tmp;
            (void)bucket->popFifo(tmp); // drop oldest to keep aligned
            return h->base_pred;
        }

        Entry recycledEntry;
        if (bucket->popFifo(recycledEntry)) {
            h->usedRecycle = true;
            h->recycledTaken = recycledEntry.taken;
            h->brType = recycledEntry.brType;

            stats.recycledPred++;

            if (h->recycledTaken != h->base_pred) {
                stats.RecycleBaseDiffer++;
            }

            DPRINTF(RecycledEntry, "Used recycled prediction for pc %#x\n",
                    pc);
            return h->recycledTaken;
        }
    }

    // Fallback to base predictor
    stats.basePred++;
    DPRINTF(RecycledEntry, "Used base prediction for pc %#x\n", pc);
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

    // Per PC stats
    auto &bs = branchStats[pc];
    bs.exec++;
    bs.taken += taken ? 1 : 0;
    RecyclingBucket *bucket = findBucket(pc);

    if (bucket) {
        // mispredict stats update
        if (squashed) {
            bs.mispred++;
            stats.missPredicts++;

            bucket->execs++;

            if (h) {
                const int recycled_pred =
                    (h->usedRecycle) ? (int)h->recycledTaken : -1;

                if (h->usedRecycle && h->base_pred == recycled_pred) {
                    stats.misPredictedBothPredictorsWrong++;
                    bucket->mispredictsTage++;
                    bucket->mispredictsRecycle++;
                } else if (h->usedRecycle && recycled_pred != (int)taken) {
                    stats.misPredictedFaultyRecycle++;
                    bucket->mispredictsRecycle++;
                    bucket->trainingMetric--;
                } else {
                    stats.missPredictedTageFault++;
                    bucket->mispredictsTage++;
                    bucket->trainingMetric++;
                }

                // Training interval
                if (bucket->execs == trainingInterval) {
                    bucket->useRecycle = (bucket->mispredictsRecycle <=
                                          bucket->mispredictsTage);
                    // For evaluation purposes two metrics were used, can be
                    // summarized for storage reduction to:
                    //   bucket->useRecycle = (bucket->trainingMetric >= 0);

                    bucket->mispredictsTage = 0;
                    bucket->mispredictsRecycle = 0;
                    bucket->execs = 0;
                }
            } else {
                stats.missPredictedTageFault++;
            }
        }
    } else {
        if (squashed) {
            bs.mispred++;
            stats.missPredicts++;
        }
    }

    // Cleanup
    if (h) {
        h->tage_bi = bi;
    } else if (made_temp && bi) {
        delete static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi);
        bi = nullptr;
    }

    // Free history
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

    using InstListener = ProbeListenerArgFunc<o3::DynInstPtr>;
    listener = cpu->getProbeManager()->connect<InstListener>(
        "ToCommit",
        [this](const o3::DynInstPtr &inst) { notifyExecutedInst(inst); });

    using PairListener =
        ProbeListenerArgFunc<std::pair<o3::DynInstPtr, o3::DynInstPtr>>;
    slistener = cpu->getProbeManager()->connect<PairListener>(
        "SquashInst",
        [this](const std::pair<o3::DynInstPtr, o3::DynInstPtr> p) {
            notifySquashedInst(p.first, p.second);
        });

    blistener = cpu->getProbeManager()->connect<PairListener>(
        "BranchDependency",
        [this](const std::pair<o3::DynInstPtr, o3::DynInstPtr> p) {
            notifyDependentInst(p.first, p.second);
        });
}

// Internal Heloers

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

    RecyclingBucket *bucket = getOrAllocBucket(pc);
    bucket->pushFifoCapped(e, bucketSize);

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
        !branch_inst->isCondCtrl() || branch_inst->seqNum == lastSeqNum) {
        return;
    }

    lastSeqNum = branch_inst->seqNum;

    const Addr pc = branch_inst->pcState().instAddr();

    RecyclingBucket *bucket = getOrAllocBucket(pc);

    const int currentDynAddrOffset = load_inst->effAddr - bucket->lastDynAddr;

    if (bucket->lastDynAddrOffset == currentDynAddrOffset) {
        bucket->strideCounterDynAddr++;

        if (bucket->strideCounterDynAddr > branchStats[pc].biggestStride) {
            branchStats[pc].biggestStride = bucket->strideCounterDynAddr;
        }
    } else if (bucket->strideCounterDynAddr > 0) {
        bucket->strideCounterDynAddr = 0;
        return;
    }

    bucket->lastDynAddr = load_inst->effAddr;
    bucket->lastDynAddrOffset = currentDynAddrOffset;
}

void
BranchRecyclingCacheDynInst::notifySquashedInst(
    const o3::DynInstPtr &mispred_inst, const o3::DynInstPtr &inst)
{
    if (!inst || inst->isSquashed() || !inst->isCondCtrl()) {
        return;
    }

    if (mispred_inst &&
        mispred_inst->pcState().instAddr() == inst->pcState().instAddr()) {}
}

void
BranchRecyclingCacheDynInst::dump(const std::string &filename)
{
    std::ofstream fileStream(simout.resolve(filename), std::ios::out);
    if (!fileStream.good()) {
        panic("Could not open %s for writing\n", filename);
    }

    ccprintf(fileStream, "pc;exec;taken;mispred;biggestStride\n");

    for (auto &bi : branchStats) {
        ccprintf(fileStream, "%llu;%i;%i;%i;%i\n",
                 (unsigned long long)bi.first, bi.second.exec, bi.second.taken,
                 bi.second.mispred, bi.second.biggestStride);
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
      ADD_STAT(misPredictedFaultyRecycle, statistics::units::Count::get(),
               "Number of times a faulty recycle caused a misprediction"),
      ADD_STAT(
          missPredictedTageFault, statistics::units::Count::get(),
          "Number of times a TAGE predictor fault caused a misprediction"),
      ADD_STAT(misPredictedBothPredictorsWrong,
               statistics::units::Count::get(),
               "Number of times both predictors were wrong causing a "
               "misprediction"),
      ADD_STAT(missPredicts, statistics::units::Count::get(),
               "Number of times a misprediction occurred"),
      ADD_STAT(committedCount, statistics::units::Count::get(),
               "Number of committed branches to bucketCache")
{}

} // namespace branch_prediction
} // namespace gem5
