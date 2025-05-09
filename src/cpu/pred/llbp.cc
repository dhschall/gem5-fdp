/*

# Copyright (c) 2025 Technical University of Munich
# Copyright (c) 2024 The University of Edinburgh
# All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

Implementation of the last-level branch predictor (LLBP).

*/

#include "cpu/pred/llbp.hh"
#include "debug/LLBP.hh"
#include <algorithm>

namespace gem5
{

namespace branch_prediction
{

LLBP::LLBP(const LLBPParams &params)
    : LTAGE(params),
      patternBufferCapacity(params.patternBufferCapacity),
      storageCapacity(params.storageCapacity),
      ctxCounterBits(params.ctxCounterBits),
      ptnCounterBits(params.ptnCounterBits),
      backingStorageLatency(params.backingStorageLatency),
      stats(this),
      rcr(3, 8, 8, 2, params.tagWidthBits)
{
    DPRINTF(LLBP, "Using experimental LLBP\n");
    DPRINTF(LLBP, "RCR: T=%d,  W=%d,  D=%d,  S=%d,  tagWidthBits=%d\n",
            rcr.T, rcr.W, rcr.D, rcr.S, params.tagWidthBits);
    DPRINTF(LLBP, "Storage: cap=%d,  bits=%d\n",
            storageCapacity, ctxCounterBits);
}

void
LLBP::init()
{
    LTAGE::init();
}

void
LLBP::squash(ThreadID tid, void *&bp_history)
{
    LLBPBranchInfo *bi = static_cast<LLBPBranchInfo *>(bp_history);
    if (bi->overridden) {
        if (!bi->avenged) {
            stats.squashedOverrides++;
        }
    }
    // TODO: No squash in LLBP?
    LTAGE::squash(tid, bp_history);
}

void
LLBP::update(ThreadID tid, Addr pc, bool taken,
             void *&bp_history, bool squashed,
             const StaticInstPtr &inst, Addr target)
{
    assert(bp_history);
    LLBPBranchInfo *bi = static_cast<LLBPBranchInfo *>(bp_history);
    LTageBranchInfo *ltage_bi = static_cast<LTageBranchInfo*>(bi);

    if (inst->isUncondCtrl()) {
        // Insert the next prefetch context into the pattern buffer
        if (backingStorage.count(rcr.getPCID()))
        {
            uint64_t pcid = rcr.getPCID();
            if (patternBuffer.count(rcr.getPCID()) == 0) {
                ++stats.prefetchesIssued;
                patternBuffer[pcid] = curCycle();

                patternBufferQueue.push_back(pcid);
                if (patternBufferQueue.size() >= patternBufferCapacity)
                {
                    patternBuffer.erase(patternBufferQueue.front());
                    patternBufferQueue.pop_front();
                    ++stats.patternBufferEvictions;
                }
            }
        }
    } else {
        if (squashed && !bi->avenged) {
            rcr.restore(bi->rcrBackup);
        } 
        storageUpdate(tid, pc, rcr.getCCID(), taken, bi);
    }

    rcr.update(pc, inst, taken);
    lTageUpdate(tid, pc, taken, ltage_bi, squashed, inst, target);

    DPRINTF(LLBP, "[thread %d] Updated %s on %lld (%s): ccid=%llu, counter=%d,"
            "uncond=%s, sz=%d\n",
            tid,
            taken ? "true" : "false",
            pc,
            inst->getName().c_str(),
            rcr.getCCID(),
            -1,
            inst->isUncondCtrl() ? "true" : "false",
            backingStorage.size());
    
    if (!squashed) {
        delete bi;
        bp_history = nullptr;
    }
}

void
LLBP::branchPlaceholder(ThreadID tid, Addr pc,
                         bool uncond, void * &bpHistory)
{
    LLBPBranchInfo *bi = new LLBPBranchInfo(*tage, *loopPredictor,
                                              pc, !uncond);
    bpHistory = (void*)(bi);
}

void
LLBP::lTageUpdate(ThreadID tid, Addr pc, bool taken, LTageBranchInfo * bi,
              bool squashed, const StaticInstPtr & inst, Addr target)
{
    assert(bi);

    if (squashed) {
        if (tage->isSpeculativeUpdateEnabled()) {
            // This restores the global history, then update it
            // and recomputes the folded histories.
            tage->squash(tid, taken, target, inst, bi->tageBranchInfo);

            if (bi->tageBranchInfo->condBranch) {
                loopPredictor->squashLoop(bi->lpBranchInfo);
            }
        }
        return;
    }

    int nrand = rng->random<int>() & 3;
    if (bi->tageBranchInfo->condBranch) {
        DPRINTF(LLBP, "Updating tables for branch:%lx; taken?:%d\n",
                pc, taken);
        tage->updateStats(taken, bi->tageBranchInfo);

        loopPredictor->updateStats(taken, bi->lpBranchInfo);

        loopPredictor->condBranchUpdate(tid, pc, taken,
            bi->tageBranchInfo->tagePred, bi->lpBranchInfo, instShiftAmt);

        tage->condBranchUpdate(tid, pc, taken, bi->tageBranchInfo,
            nrand, target, bi->lpBranchInfo->predTaken);
    }

    tage->updateHistories(tid, pc, false, taken, target,
                           inst, bi->tageBranchInfo);

}

Prediction
LLBP::predict(ThreadID tid, Addr branch_pc, bool cond_branch, void *&b)
{
    Addr pc = branch_pc;
    
    LLBPBranchInfo *bi = new LLBPBranchInfo(*tage, *loopPredictor, pc, cond_branch);
    LTageBranchInfo *ltage_bi = static_cast<LTageBranchInfo *>(bi);
    b = (void*)(bi);

    Prediction ltage_prediction = lTagePredict(
        tid, branch_pc, cond_branch, ltage_bi
    );
    
    auto tage_bi = bi->tageBranchInfo;
    bi->overridden = false;
    bi->base_pred = ltage_prediction.taken;

    int8_t llbp_confidence = 0;

    if (cond_branch)
    {
        rcr.backup(bi->rcrBackup);
        int tage_bank = tage->nHistoryTables;
        if (tage_bi->provider == TAGEBase::TAGE_LONGEST_MATCH)
            tage_bank = tage_bi->hitBank;
        if (tage_bi->provider == TAGEBase::TAGE_ALT_MATCH)
            tage_bank = tage_bi->altBank;
        auto ccid = rcr.getCCID();
        if (backingStorage.count(ccid))
        {
            if (patternBuffer.count(ccid)) {
                Cycles additionalLatency = calculateRemainingLatency(patternBuffer[ccid]);
                if (additionalLatency == 0) {
                    int i = findBestPattern(tid, pc, backingStorage[ccid]);
                    if (i >= 0)
                    {
                        int key = tage->gtag(tid, pc, i);
                        ++stats.demandHitsTotal;
                        llbp_confidence = backingStorage[ccid].patterns[key].counter;
                        bool llbp_prediction = llbp_confidence >= 0;
                        if (i > tage_bank)
                        {
                            ++stats.demandHitsOverride;
                            bi->overridden = true;
                            bi->llbp_pred = llbp_prediction;
                        } else {
                            ++stats.demandHitsNoOverride;
                        }
                    } else {
                        ++stats.demandMissesNoPattern;
                        ++stats.demandMissesTotal;
                    }
                } else {
                    ++stats.demandMissesTotal;
                }
            } else {
                ++stats.demandMissesNoPrefetch;
                ++stats.demandMissesTotal;
            }
        } else {
            ++stats.demandMissesCold;
            ++stats.demandMissesTotal;
        }
    }
    Cycles latency = ltage_prediction.latency;
    return Prediction {.taken = bi->getPrediction(), .latency = latency};
}


//prediction
Prediction
LLBP::lTagePredict(ThreadID tid, Addr branch_pc, bool cond_branch, LTageBranchInfo* bi)
{
    bool pred_taken = tage->tagePredict(tid, branch_pc, cond_branch,
                                        bi->tageBranchInfo);

    pred_taken = loopPredictor->loopPredict(tid, branch_pc, cond_branch,
                                            bi->lpBranchInfo, pred_taken,
                                            instShiftAmt);
    if (cond_branch) {
        if (bi->lpBranchInfo->loopPredUsed) {
            bi->tageBranchInfo->provider = LOOP;
        }
        DPRINTF(LLBP, "Predict for %lx: taken?:%d, loopTaken?:%d, "
                "loopValid?:%d, loopUseCounter:%d, tagePred:%d, altPred:%d\n",
                branch_pc, pred_taken, bi->lpBranchInfo->loopPred,
                bi->lpBranchInfo->loopPredValid,
                loopPredictor->getLoopUseCounter(),
                bi->tageBranchInfo->tagePred, bi->tageBranchInfo->altTaken);
    }

    // record final prediction
    bi->lpBranchInfo->predTaken = pred_taken;

    return staticPrediction(pred_taken);
}


int8_t LLBP::absPredCounter(int8_t counter)
{
    return counter >= 0 ? counter : -counter - 1;
}

/**
 * Update LLBP with the real outcome of a branch.
 * It is currently assumed that the PB still contains the
 * corresponding pattern set (no access latency applied).
 * The context is created if it does not exist yet.
 * The pattern is updated / a longer pattern is allocated.
 *
 * @param tid Thread ID
 * @param pc Program counter
 * @param cid Context ID
 * @param taken Whether the branch was taken
 * @param bi Branch info
 */
void LLBP::storageUpdate(ThreadID tid, Addr pc, uint64_t cid, bool taken,
                         LLBPBranchInfo *bi)
{
    // Check whether the current context is known
    // If not, we create a new context
    if (backingStorage.count(cid))
    {
        auto context = backingStorage[cid];
        // The best pattern is always the one that was used to predict
        // the branch. Both the pattern and the context are updated.
        int i = findBestPattern(tid, pc, context);
        if (i >= 0)
        {
            int key = tage->gtag(tid, pc, i);
            TAGEBase::ctrUpdate(context.patterns[key].counter, taken,
                                ptnCounterBits);
            if (context.patterns[key].counter == (taken ? 1 : -2))
            {
                // Context is now medium confidence
                TAGEBase::unsignedCtrUpdate(backingStorage[cid].confidence, true,
                                            ctxCounterBits);
            }
            else if (context.patterns[key].counter == (taken ? -1 : 0))
            {
                // Context is now low confidence
                TAGEBase::unsignedCtrUpdate(context.confidence, false,
                                            ctxCounterBits);
            }
        }

        // If a misprediction occurs, we allocate a new pattern with longer history
        // in the context. The pattern with the weakest confidence is replaced.
        // This only happens if the LLBP was the provider.
        if (bi->overridden) {
            if (bi->getPrediction() != taken) {
                if (!bi->avenged) {
                    ++stats.wrongOverridesTotal;
                    if (bi->llbp_pred == bi->base_pred) {
                        ++stats.wrongOverridesIdentical;
                    }
                    bi->avenged = true;
                }
                if (i < tage->nHistoryTables - 1) {
                    int choosei = std::min(i + 1, (int) tage->nHistoryTables - 1);
                    context.patterns[tage->gtag(tid, pc, choosei)].counter = taken ? 0 : -1;
                }
            } else {
                ++stats.correctOverridesTotal;   
                if (bi->llbp_pred == bi->base_pred) {
                    ++stats.correctOverridesIdentical;
                }
            }
        }
    }
    else
    {
        while (backingStorage.size() >= storageCapacity)
        {
            ++stats.backingStorageEvictions;
            uint64_t i = findVictimContext();
            backingStorage.erase(i);
        }

        ++stats.backingStorageInsertions;
        backingStorage[cid] = LLBP::Context();
        // Fill the context with initial patterns
        // (only this branch, in different history lengths)
        for (int i = 1; i <= tage->nHistoryTables; i++)
        {
            backingStorage[cid].patterns[tage->gtag(tid, pc, i)].counter = taken ? 0 : -1;
        }
    }
}

/**
 * Find the best (=longest) pattern in the context.
 * The context is searched by comparing the pc tag with decreasing length
 * (similar to the TAGE predictor).
 *
 * @param tid Thread ID
 * @param pc Program counter
 * @param ctx Context to search in
 * @return Index of the best pattern, or -1 if not found
 */
int LLBP::findBestPattern(ThreadID tid, Addr pc, Context &ctx)
{
    for (int i = tage->nHistoryTables; i > 0; i--)
    {
        int key = tage->gtag(tid, pc, i);
        if (ctx.patterns.count(key))
        {
            return i;
        }
    }
    return -1;
}

/**
 * Find the pattern with the lowest confidence in the context.
 * Lengths and confidence vals smaller than {min} are not considered.
 *
 * @param min Minimum confidence value
 * @param ctx Context to search in
 * @return Index of the pattern with the lowest confidence, or -1 if not found
 */
int LLBP::findVictimPattern(int min, Context &ctx)
{
    int min_conf = 1 << (ptnCounterBits - 2);
    for (int i = (min >= 0 ? min + 1 : 1); i <= tage->nHistoryTables; i++)
    {
        if (absPredCounter(ctx.patterns[i].counter) < min_conf)
        {
            return i;
        }
    }
    return -1;
}

/**
 * Find the context with the lowest confidence.
 * @return Index of the context with the lowest confidence, or -1 if not found
 */
uint64_t LLBP::findVictimContext()
{
    auto elem = std::min_element(
        backingStorage.begin(), backingStorage.end(),
        [](const auto &a, const auto &b)
        {
            return a.second.confidence < b.second.confidence;
        });
    if (elem == backingStorage.end())
    {
        return -1;
    }
    return elem->first;
}

Cycles LLBP::calculateRemainingLatency(Cycles insertTime) {
    Cycles passedTime = (curCycle() - insertTime);
    if (passedTime >= backingStorageLatency)
        return Cycles(0);
    return backingStorageLatency - passedTime;
}

/* from LLBP source code: */

LLBP::RCR::RCR(int _T, int _W, int _D, int _shift, int _CTWidth)
    : tagWidthBits(_CTWidth), T(_T), W(_W), D(_D), S(_shift)
{
    bb.resize(maxwindow);
    ctxs = {0, 0};
}

/**
 * Given {n} number of branches starting from the end of the RCR (front of the vec)
 * (minus {skip} # of branches) we create the hash function by shifting
 * each PC by {shift} number if bits i.e.
 *
 *   000000000000|  PC  |    :vec[end-skip]
 * ^ 0000000000|  PC  |00    :vec[end-skip-1]
 * ^ 00000000|  PC  |0000    :vec[end-skip-2]
 *           .                     .
 *           .                     .
 *           .                     .
 * ^ |  PC  |000000000000    :vec[end-skip-n-1]
 * ----------------------
 *       final hash value
 *  Then, the hash value is wrapped to the size of the context tag:
 *  @return final hash value % 2^tagWidthBits
*/
uint64_t
LLBP::RCR::calcHash(std::list<uint64_t> &vec, int n,
    int skip, int shift)
{
    uint64_t hash = 0;
    if (vec.size() < (skip + n))
    {
        return 0;
    }

    // Compute the rolling hash in element order (newer branches at the front)
    uint64_t sh = 0;
    auto it = vec.begin();
    std::advance(it, skip);
    for (; (it != vec.end()) && (n > 0); it++, n--)
    {
        uint64_t val = *it;

        // Shift the value
        hash ^= val << uint64_t(sh);

        sh += shift;
        if (sh >= tagWidthBits)
        {
            sh -= uint64_t(tagWidthBits);
        }
    }
    return moduloTwoExp(hash, tagWidthBits);
}

uint64_t LLBP::RCR::getCCID()
{
    return moduloTwoExp(ctxs.ccid, tagWidthBits);
}    // Hash of all branches

uint64_t LLBP::RCR::getPCID()
{
    return moduloTwoExp(ctxs.pcid, tagWidthBits);
}

bool LLBP::RCR::update(Addr pc, const StaticInstPtr &inst, bool taken)
{
    bool update = false;

    switch (T)
    {
    case 0: // All branches
        update = true;
        break;

    case 1: // Only calls
        if (inst->isCall()) update = true;
        break;

    case 2: // Only calls and returns
        if (inst->isCall() || inst->isReturn())
            update = true;
        break;

    case 3: // Only unconditional branches
        if (inst->isUncondCtrl()) update = true;
        break;

    case 4: // All taken branches
        if (taken) update = true;
        break;
    }

    if (update)
    {
        // Add the new branch to the history
        bb.push_front(pc);

        // Remove the oldest branch
        bb.pop_back();

        // The current context.
        ctxs.ccid = calcHash(bb, W, D, S);
        // The prefetch context.
        ctxs.pcid = calcHash(bb, W, 0, S);

        return true;
    }

    return false;
}

void LLBP::RCR::backup(std::list<uint64_t> &vec)
{
    vec.clear();
    int count = 0;
    for (auto it = bb.begin(); it != bb.end(); ++it)
    {
        if (count == D + W - 1) {
            break;
        }
        vec.push_back(*it);
        count++;
    }
}

void LLBP::RCR::restore(std::list<uint64_t> &vec)
{
    bb.clear();
    for (auto it = vec.begin(); it != vec.end(); ++it)
    {
        bb.push_back(*it);
    }
    // The current context.
    ctxs.ccid = calcHash(bb, W, D, S);
    // The prefetch context.
    ctxs.pcid = calcHash(bb, W, 0, S);
}

LLBP::LLBPStats::LLBPStats(LLBP *llbp)
    : statistics::Group(llbp),
      ADD_STAT(prefetchesIssued, statistics::units::Count::get(),
              "Number of prefetches issued to the backing storage"),
      ADD_STAT(demandHitsTotal, statistics::units::Count::get(),
              "Total on-demand hits to the pattern buffer"),
      ADD_STAT(demandHitsOverride, statistics::units::Count::get(),
              "On-demand hits to the pattern buffer with LLBP overriding the base predictor"),
      ADD_STAT(demandHitsNoOverride, statistics::units::Count::get(),
              "On-demand hits to the pattern buffer, using the base predictor (LLBP dropped)"),
      ADD_STAT(demandMissesTotal, statistics::units::Count::get(),
              "Total on-demand misses to the pattern buffer"),
      ADD_STAT(demandMissesNoPattern, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer, the chosen pattern-set did not contain the needed pattern"),
      ADD_STAT(demandMissesNoPrefetch, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer where the context was not scheduled for insertion"),
      ADD_STAT(demandMissesCold, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer where the context was not in the backing storage"),
      ADD_STAT(patternBufferEvictions, statistics::units::Count::get(),
              "Number of pattern sets evicted from the pattern buffer due to capacity limits"),
      ADD_STAT(backingStorageEvictions, statistics::units::Count::get(),
              "Number of pattern sets evicted from the backing storage due to capacity limits"),
      ADD_STAT(backingStorageInsertions, statistics::units::Count::get(),
              "Number of pattern sets inserted into the backing storage (including replacements)"),
      ADD_STAT(correctOverridesTotal, statistics::units::Count::get(),
              "Number of branches predicted correctly by LLBP (LLBP was provider)"),
      ADD_STAT(correctOverridesIdentical, statistics::units::Count::get(),
              "Number of branches predicted correctly by LLBP, but the base predictor would also be correct"),
      ADD_STAT(wrongOverridesTotal, statistics::units::Count::get(),
              "Number of branches predicted wrong by LLBP (LLBP was provider)"),
      ADD_STAT(wrongOverridesIdentical, statistics::units::Count::get(),
              "Number of branches predicted wrong by LLBP, but the base predictor would also be wrong"),
      ADD_STAT(squashedOverrides, statistics::units::Count::get(),
              "Number of branches predicted by LLBP, but squashed before the outcome was known")
              {}

} // namespace branch_prediction
} // namespace gem5
