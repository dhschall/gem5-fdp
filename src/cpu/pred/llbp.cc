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
    : ConditionalPredictor(params),
      base(params.base),
      backingStorageCapacity(params.backingStorageCapacity),
      backingStorageLatency(params.backingStorageLatency),
      patternSetCapacity(params.patternSetCapacity),
      patternSetAssoc(params.patternSetAssoc),
      patternSetBankBits(/* Bits for bucket */ patternSetAssoc ? ceilLog2(patternSetAssoc) : 0 +
                         /* Bits for bank (36) */ ceilLog2(36)),
      contextCounterWidth(params.contextCounterWidth),
      patternCounterWidth(params.patternCounterWidth),
      lightningPredEnabled(params.lightningPredEnabled),
      lightningPredCutoff(params.lightningPredCutoff),
      stats(this),
      backingStorage(),
      patternBuffer(params.patternBufferCapacity,
        params.patternBufferAssoc,
        this->backingStorage,
        stats.patternBufferEvictions),
    TTWidth(params.patterTagBits),
    optimalPrefetching(backingStorageLatency == Cycles(0)),
      rcr(params.rcrType,
        params.rcrWindow,
        params.rcrDist,
        params.rcrShift,
        params.rcrTagWidth)
{
    // assert(floorLog2(patternSetAssoc)
    // TODO: Add assert to check that the base predictor is of type LLBP_TAGE_64KB
    static_cast<LLBP_TAGE_64KB *>(base->tage)->setParent(this);
    DPRINTF(LLBP, "Using experimental LLBP\n");
    DPRINTF(LLBP, "RCR: T=%d,  W=%d,  D=%d,  S=%d,  tagWidthBits=%d\n",
            rcr.T, rcr.W, rcr.D, rcr.S, params.rcrTagWidth);
    DPRINTF(LLBP, "Storage: cap=%d,  bits=%d\n",
            backingStorageCapacity, contextCounterWidth);
}

void
LLBP::init()
{
    // First initialize the base predictor
    base->tage->init();

    // for (int i = 1; i <= base->getNumHistoryTables(); i++) {
    //     auto m = (i%2) ? base->tage->histLengths[i] : base->tage->histLengths[i]+2;
    //     fghrT1[i].init(m, TTWidth);
    //     fghrT2[i].init(m, TTWidth - 1);
    //     printf("T1[%d]: HistLen=%d, Width=%d\n",
    //            i, m, TTWidth);
    // }

    fltTables.resize(base->getNumHistoryTables() + 1, 0);

#define FILTER_TABLES
#ifdef FILTER_TABLES
    // LLBP does not provide for all different history lenghts in
    // TAGE a prediction only for the following once which where
    // empirically determined. Note this
    // are not the actual length but the table indices in TAGE.
    auto l = {6,10,13,14,15,16,17,18,  19,20,22,24,26,28,32,36};

#else
    std::list<int> l;
    for (int i = 1; i <= base->getNumHistoryTables(); i++) {
        if (base->tage->noSkip[i]) {
            l.push_back(i);
        }
    }
#endif //FILTER_TABLES

    int n = 0;
    for (auto i : l) {
        // To reduce the complexity of the multiplexer LLBP groups
        // always four consecutive history lenght in one bucket.
        // As the pattern sets are implemented a set associative
        // structure the lower bits determine the set=bucket.
        // The `fltTable`-map not only filters the history lengths
        // but also maps each length the correct pattern set index.
        // E.e. for the four way associativity the following function
        // ensures that history length 6,10,13,14 gets assign
        // 0,4,8,12 with the lowest two bits 0b00. Thus, the set will
        // be the same.
        auto pa = patternSetAssoc ? patternSetAssoc : 1;
        auto bucket = n / pa;
        fltTables[i] = ((i) << ceilLog2(pa) ) | bucket;
        printf("%i=>%i:%i:%i ", i, n, bucket, fltTables[i]);
        n++;
    }
    printf("\n");

}

void
LLBP::squash(ThreadID tid, void *&bp_history)
{
    LLBPBranchInfo *bi = static_cast<LLBPBranchInfo *>(bp_history);
    if (bi->overridden) {
        stats.squashedOverrides++;
    }
    base->squash(tid, bi->ltage_bi);
    delete bi;
    bp_history = nullptr;
}

void
LLBP::update(ThreadID tid, Addr pc, bool taken,
             void *&bp_history, bool resteer,
             const StaticInstPtr &inst, Addr target)
{
    assert(bp_history);
    LLBPBranchInfo *bi = static_cast<LLBPBranchInfo *>(bp_history);
    TAGE_SC_L::TageSCLBranchInfo *tage_bi = static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi->ltage_bi);

    if (resteer) {
        if (bi->rcrBackup.size()) {
            rcr.restore(bi->rcrBackup);
            rcr.update(pc >> instShiftAmt, inst, taken);
        }

        patternBuffer.clearInFlight(curCycle(), backingStorageLatency);

        // base->update(tid, pc, taken, bi->ltage_bi, resteer, inst, target);
        base->update(tid, pc, taken, tage_bi, resteer, inst, target);
        return;
    }

    // This is a bit a hackish way to communicate the LLBP override information
    // to the base predictor. The base predictor will use the current bi
    // in its update and allocation functions
    curUpdateBi = bi;

    // Do the base predictor update.
    base->update(tid, pc, taken, tage_bi, resteer, inst, target);


    if (inst->isCondCtrl())
        storageUpdate(tid, pc, taken, bi);


    std::string rcr_cont = "";

    for (auto v: rcr.bb)
        rcr_cont.append(std::to_string(v) + " | ");


    DPRINTF(LLBP, "LLBP::%s(pc=%llx, inst=%s, taken=%i, resteer=%i): "
            "ccid=%llu, uncond=%i, sz=%d, RCR: %s\n",
            __func__,
            pc, inst->getName().c_str(),
            taken, resteer,
            bi->cid,
            inst->isUncondCtrl(),
            backingStorage.size(),
            rcr_cont
        );

    // auto& tHist = base->tage->threadHistory[tid];
    // for (int n = tage_bi->tageBranchInfo->nGhist; n > 0; n--) {
    //     bool bit = *(tHist.gHist+n-1);
    //     DPRINTF(LLBP, "B:%i\n", bit);
    //     for (int i = 1; i <= base->getNumHistoryTables(); i++) {
    //         if (base->tage->noSkip[i]) {
    //             fghrT1[i].update(tHist.gHist+n-1);
    //             fghrT2[i].update(tHist.gHist+n-1);
    //         }
    //     }
    // }

    branchCount++;
    delete tage_bi;

    delete bi;
    bp_history = nullptr;
}

void
LLBP::branchPlaceholder(ThreadID tid, Addr pc,
                         bool uncond, void * &bpHistory)
{
    LLBPBranchInfo *bi = new LLBPBranchInfo(pc, !uncond);
    base->branchPlaceholder(tid, pc, uncond, bi->ltage_bi);
    bpHistory = (void*)(bi);
}

Prediction
LLBP::lookup(ThreadID tid, Addr pc, void *&bp_history)
{
    Prediction retval = predict(tid, pc, true, bp_history);
    return retval;
}

void
LLBP::calculateKeys(Addr pc)
{
    // TODO: Not needed anymore
    std::string s = "";
    for (int i = 1; i <= base->getNumHistoryTables(); i++) {
        // if (base->tage->noSkip[i]) {
        if (fltTables[i] > 0) {
            uint64_t key = pc >> instShiftAmt;
            key ^= fghrT1[i].comp ^ (fghrT2[i].comp << 1);
            key &= ((1ULL << uint64_t(TTWidth)) - 1ULL);
            KEY[i] = uint64_t(key) << 10ULL | uint64_t(fltTables[i]);
            s.append(std::to_string(i) + ":" + std::to_string(KEY[i]) + " | ");
        }
    }
    DPRINTF(LLBP, "LLBP::%s(pc=%#llx): Keys: %s\n", __func__, pc, s);
}

uint64_t
LLBP::calculateKey(TAGEBase::BranchInfo* tageBi, int tageBank, Addr pc)
{
    uint64_t key = 0;
    uint64_t tag = tageBi->tableTags[tageBank];
    uint64_t index = tageBi->tableIndices[tageBank];
    // Align the index to the upper bits of the key
    // TODO: Parametrize this
    // 10 bits is the number of bits used in the TAGEBase::gindex
    index <<= uint64_t(TTWidth - 10);
    key ^= tag ^ index;
    key &= ((1ULL << uint64_t(TTWidth)) - 1ULL);
    return uint64_t(key) << 10 | uint64_t(fltTables[tageBank]);
}

Prediction
LLBP::predict(ThreadID tid, Addr branch_pc, bool cond_branch, void *&b)
{
    Addr pc = branch_pc;

    LLBPBranchInfo *bi = new LLBPBranchInfo(pc, cond_branch);

    Prediction ltage_prediction = base->predict(
        tid, branch_pc, cond_branch, bi->ltage_bi
    );

    b = (void*)(bi);

    TAGE_SC_L::TageSCLBranchInfo *scltage_bi = static_cast<TAGE_SC_L::TageSCLBranchInfo*>(bi->ltage_bi);

    auto tage_bi = scltage_bi->tageBranchInfo;
    bi->overridden = false;
    bi->base_pred = ltage_prediction.taken;

    bool lightningOverride = false;

    Cycles latency = ltage_prediction.latency;

    int8_t llbp_confidence = 0;

    if (cond_branch)
    {
        int tage_bank = 0;

        if (tage_bi->provider == TAGEBase::TAGE_LONGEST_MATCH)
            tage_bank = tage_bi->hitBank;
        if (tage_bi->provider == TAGEBase::TAGE_ALT_MATCH)
            tage_bank = tage_bi->altBank;
        if (tage_bi->provider == TAGE_SC_L::LOOP || tage_bi->provider == TAGE_SC_L::SC)
            tage_bank = base->getNumHistoryTables();
        if (tage_bank)
           ++stats.baseHitsTotal;

        auto ccid = rcr.getCCID();
        bi->index = 0;
        bi->cid = ccid;
        if (backingStorage.count(ccid)) {
            auto& context = backingStorage.at(ccid);
            PatternBufferEntry* pbe = patternBuffer.get(ccid);
            if (pbe) {
                auto& entry = *pbe;
                int bestPattern = findBestPattern(context, tage_bi, branch_pc);
                bi->index = bestPattern >= 0 ? bestPattern : 0; 
                Cycles additionalLatency = calculateRemainingLatency(entry.insertTime);
                if (additionalLatency == 0) {
                    if (bi->index > 0)
                    {
                        uint64_t key = calculateKey(tage_bi, bi->index, branch_pc);
                        auto &pattern = *context.patterns.getEntry(key);

                        context.patterns.wasHit(key);
                        entry.lastUsed = curCycle();

                        ++stats.demandHitsTotal;
                        llbp_confidence = pattern.counter;
                        bool llbp_prediction = llbp_confidence >= 0;

                        // Override early if lightning is enabled
                        if (PatternSet::absConfidence(llbp_confidence) > lightningPredCutoff) {
                            bi->lightningTarget = true;
                            bi->llbp_pred = llbp_prediction;
                            if (lightningPredEnabled) {
                                lightningOverride = true;
                                bi->overridden = true;
                                latency = Cycles(0);
                                ++stats.lightningHitsTotal;
                            }
                        }

                        if (bi->index >= tage_bank && !lightningOverride) {
                            ++stats.demandHitsOverride;
                            bi->overridden = true;
                            bi->llbp_pred = llbp_prediction;
                        } else if (!lightningOverride) {
                            ++stats.demandHitsNoOverride;
                        }
                    } else {
                        ++stats.demandMissesPatternMiss;
                        ++stats.demandMissesTotal;
                    }
                } else {
                    ++stats.demandMissesTotal;
                }
            } else {
                ++stats.demandMissesContextNotPrefetched;
                ++stats.demandMissesTotal;
            }

        } else {
            ++stats.demandMissesContextUnknown;
            ++stats.demandMissesTotal;
        }
        DPRINTF(LLBP, "LLBP::%s(pc=%#llx): CID=%lx, Base:[Hit=%i,p=%i] LLBP:[Hit=%i,p=%i] confidence=%d, overridden=%s\n",
            __func__,
            pc,
            bi->cid,
            tage_bi->hitBank, bi->base_pred,
            bi->index, bi->llbp_pred,
            llbp_confidence,
            bi->overridden);
    }

    if (bi->overridden) {
        // Overridden prediction
        tage_bi->tagePred = bi->llbp_pred;
        tage_bi->longestMatchPred = tage_bi->altTaken = bi->llbp_pred;
        tage_bi->hitBank = tage_bi->altBank = 0;
        tage_bi->provider = TAGEBase::BIMODAL_ONLY;
        scltage_bi->lpBranchInfo->predTaken = bi->llbp_pred;
        scltage_bi->lpBranchInfo->loopPredUsed = false;
        scltage_bi->scBranchInfo->usedScPred = false;
    }

    return Prediction {.taken = bi->getPrediction(), .latency = latency};
}

void
LLBP::updateHistories(
    ThreadID tid, Addr pc, bool uncond,
    bool taken, Addr target,
    const StaticInstPtr &inst,
    void *&bp_history)
{
    LLBPBranchInfo *bi;
    if (bp_history == nullptr) {
        assert(uncond);
        bi = new LLBPBranchInfo(pc, !uncond);
        bp_history = (void*)(bi);
    } else {
        bi = static_cast<LLBPBranchInfo *>(bp_history);
    }


    // Backup the RCR state in case we need to restore it.
    rcr.backup(bi->rcrBackup);

    // Update the RCR with the current branch
    if (rcr.update(pc >> instShiftAmt, inst, taken)) {

        // If the RCR has updated the context ID, we need to
        // check whether we have to prefetch the context.
        uint64_t pcid = rcr.getPCID();
        if (backingStorage.count(pcid))
        {
            if (patternBuffer.get(pcid) == nullptr)
            {
                ++stats.prefetchesIssued;
                patternBuffer.insert(pcid, curCycle());
            }
        }
    }

    base->updateHistories(tid, pc, uncond, taken, target, inst, bi->ltage_bi);
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
 * @param taken Whether the branch was taken
 * @param bi Branch info
 */
void LLBP::storageUpdate(ThreadID tid, Addr pc, bool taken, LLBPBranchInfo *bi)
{
    // TAGE_SC_L::TageSCLBranchInfo *ltage_bi =
    //     static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi->ltage_bi);

    uint64_t cid = bi->cid;


    auto tage_bi = static_cast<TAGE_SC_L::TageSCLBranchInfo *>(bi->ltage_bi)->tageBranchInfo;

    DPRINTF(LLBP, "LLBP::%s(pc=%lx, taken=%i) "
            "cid=%llu, index=%d, "
            "prediction=%d, mispred=%d, "
            "llbp_pred=%d, base_pred=%d, overridden=%i\n",
            __func__, pc, taken,
            cid, bi->index,
            bi->getPrediction(), taken != bi->getPrediction(),
            bi->llbp_pred, bi->base_pred, bi->overridden);



    /**************************************************
     * Allocation
     *
     * If the branch was mispredicted, we allocate a new pattern
     * with longer history in the context.
     * The pattern with the weakest confidence is replaced.
     ***************************************************/
    // if (bi->getPrediction() != taken) {
    auto& alloc_banks = static_cast<LLBP_TAGE_64KB *>(base->tage)->alloc_banks;
    if (alloc_banks.size() > 0) {

        // Check if the context already exists
        if (!backingStorage.count(cid)) {

            // If not, we create a new context
            while (backingStorage.size() >= backingStorageCapacity)
            {
                ++stats.backingStorageEvictions;
                uint64_t i = findVictimContext();
                backingStorage.erase(i);
            }
            DPRINTF(LLBP, "LLBP: CTX Alloc:%llx,\n", cid);
            // backingStorage.emplace(cid, Context(PatternSet(64*8, 64*8, 8, stats)));
            if (patternSetCapacity == 0) {
                backingStorage.emplace(cid, Context(PatternSet(patternSetBankBits, stats)));
            } else {
                // FIX: The Associativity is actually the set size, not the associativity
                backingStorage.emplace(cid, Context(PatternSet(
                    patternSetCapacity, patternSetAssoc, patternSetBankBits/* TODO Remove! */, stats
                )));
            }
            ++stats.backingStorageInsertions;
        }

        Context& context = backingStorage.at(cid);

        // uint64_t key = context.patterns.calculateKey(tage_bi, tage_bank, pc);
        // context.patterns.insertEntry(key, taken);

        for (auto bank : alloc_banks) {
            if (fltTables[bank]) {
                uint64_t key = calculateKey(tage_bi, bank, pc);
                // uint64_t key = KEY[j];
                DPRINTF(LLBP, "LLBP Alloc:%i, %llx\n", bank, key);
                ++stats.allocationsTotal;
                context.patterns.insertEntry(key, taken);
            }
        }
    }






    /**************************************************
     * Update
     *
     */

    // Update the gem5 statistics for override tracking
    if (bi->overridden) {
        if (bi->getPrediction() != taken) {
            ++stats.wrongOverridesTotal;
            if (bi->llbp_pred == bi->base_pred) {
                ++stats.wrongOverridesIdentical;
            }
        } else {
            ++stats.correctOverridesTotal;
            if (bi->llbp_pred == bi->base_pred) {
                ++stats.correctOverridesIdentical;
            }
        }
    }

    // Lightning predictions are counted as regrets if LLBP would have differed from base,
    // and LLBP's guess would have been wrong (waiting for base would have saved a misprediction)
    if (bi->lightningTarget) {
        if (bi->llbp_pred != bi->base_pred
        && bi->llbp_pred != taken) {
            ++stats.lightningHitsRegret;
        }
    }

    // Check whether the branch context is known
    // If not, we create a new context
    if (backingStorage.count(cid))
    {
        LLBP::Context& context = backingStorage.at(cid);

        // int i = bi->index;
        if (bi->index > 0 && bi->overridden)
        {
            uint64_t key = calculateKey(tage_bi, bi->index, pc);
            // uint64_t key = KEY[i];
            LLBP::Pattern* p = context.patterns.getEntry(key);

            if (p) {
                LLBP::Pattern& pattern = *p;

                int8_t conf_before = pattern.counter;
                TAGEBase::ctrUpdate(pattern.counter, taken, patternCounterWidth);
                int8_t conf_after = pattern.counter;

                DPRINTF(LLBP, "LLBP::%s() CID=%llx key=%llx: %d -> %d (%s)\n",
                        __func__, cid, key, conf_before, conf_after, taken ? "taken" : "not taken");
                // This function updates the context replacement counter
                // - If a pattern becomes confident (correct prediction)
                //   the replacement counter is increased
                // - If a pattern becomes low confident (incorrect prediction)
                //   the replacement counter is decreased
                if (pattern.counter == (taken ? 1 : -2))
                {
                    // Context is now medium confidence
                    TAGEBase::unsignedCtrUpdate(context.confidence, true,
                                                contextCounterWidth);
                }
                else if (pattern.counter == (taken ? -1 : 0))
                {
                    // Context is now low confidence
                    TAGEBase::unsignedCtrUpdate(context.confidence, false,
                                                contextCounterWidth);
                }

                if (bi->getPrediction() == taken) {
                    context.patterns.wasUseful(key);
                }
            }
        }


        // // If a misprediction occurs, we allocate a new pattern with longer history
        // // in the context. The pattern with the weakest confidence is replaced.
        // if (bi->getPrediction() != taken) {
        //     if (i < base->getNumHistoryTables()) {
        //         ++stats.allocationsTotal;
        //         i = i+1;
        //         while (!base->tage->noSkip[i] && i < base->getNumHistoryTables())
        //             i = i+1;
        //         uint64_t key = context.patterns.calculateKey(tage_bi, i, pc);
        //         context.patterns.insertEntry(key, taken);
        //     }
        // }
    // }
    // else
    // {
    //     while (backingStorage.size() >= backingStorageCapacity)
    //     {
    //         ++stats.backingStorageEvictions;
    //         uint64_t i = findVictimContext();
    //         backingStorage.erase(i);
    //     }

    //     // TODO: Check if this is a skip table
    //     int tage_bank = 1;
    //     if (tage_bi->provider == TAGEBase::TAGE_LONGEST_MATCH)
    //         tage_bank = tage_bi->hitBank;
    //     if (tage_bi->provider == TAGEBase::TAGE_ALT_MATCH)
    //         tage_bank = tage_bi->altBank;


    //     if (patternSetCapacity == 0) {
    //         backingStorage.emplace(cid, Context(PatternSet(patternSetBankBits, stats)));
    //     } else {
    //         backingStorage.emplace(cid, Context(PatternSet(
    //             patternSetCapacity, patternSetAssoc, patternSetBankBits, stats
    //         )));
    //     }

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
int LLBP::findBestPattern(Context &ctx, TAGEBase::BranchInfo *bi, Addr pc)
{
    for (int i = base->getNumHistoryTables(); i > 0; i--)
    {
        // if (!base->tage->noSkip[i]) continue;
        if (fltTables[i] == 0) continue;
        uint64_t key = calculateKey(bi, i, pc);
        // uint64_t key = KEY[i];
        if (ctx.patterns.getEntry(key))
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
LLBP::RCR::calcHash(int n, int skip, int shift)
{
    uint64_t hash = 0;
    if (bb.size() < (skip + n))
    {
        return 0;
    }

    // Compute the rolling hash in element order (newer branches at the front)
    uint64_t sh = 0;
    auto it = bb.begin();
    std::advance(it, skip);
    for (; (it != bb.end()) && (n > 0); it++, n--)
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
    return ctxs.ccid;
}    // Hash of all branches

uint64_t LLBP::RCR::getPCID()
{
    return ctxs.pcid;
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
        ctxs.ccid = calcHash(W, D, S);
        // The prefetch context.
        ctxs.pcid = calcHash(W, 0, S);


        return true;
    }

    return false;
}

void LLBP::RCR::backup(std::list<uint64_t>& vec)
{
    vec.clear();
    int count = D + W;
    for (auto it = bb.begin();
        (it != bb.end()) && (count > 0);
        ++it, --count)
    {
        vec.push_back(*it);
    }
}

void LLBP::RCR::restore(std::list<uint64_t>& vec)
{
    bb.clear();
    for (auto it = vec.begin(); it != vec.end(); ++it)
    {
        bb.push_back(*it);
    }
    // The current context.
    ctxs.ccid = calcHash(W, D, S);
    // The prefetch context.
    ctxs.pcid = calcHash(W, 0, S);
}

LLBP::LLBPStats::LLBPStats(LLBP *llbp)
    : statistics::Group(llbp),
      parent(llbp),
      ADD_STAT(allocationsTotal, statistics::units::Count::get(),
              "Total number of new patterns allocated in any pattern set"),
      ADD_STAT(prefetchesIssued, statistics::units::Count::get(),
              "Number of prefetches issued to the backing storage"),
      ADD_STAT(baseHitsTotal, statistics::units::Count::get(),
              "Total on-demand hits of the base predictor"),
      ADD_STAT(demandHitsTotal, statistics::units::Count::get(),
              "Total on-demand hits to the pattern buffer"),
      ADD_STAT(demandHitsOverride, statistics::units::Count::get(),
              "On-demand hits to the pattern buffer with LLBP overriding the base predictor"),
      ADD_STAT(demandHitsNoOverride, statistics::units::Count::get(),
              "On-demand hits to the pattern buffer, using the base predictor (LLBP dropped)"),
      ADD_STAT(demandMissesTotal, statistics::units::Count::get(),
              "Total on-demand misses to the pattern buffer"),
      ADD_STAT(demandMissesPatternMiss, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer, the chosen pattern-set did not contain the needed pattern"),
      ADD_STAT(demandMissesContextTooLate, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer where the context was still delayed from insertion latency"),
      ADD_STAT(demandMissesContextNotPrefetched, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer where the context was not scheduled for insertion"),
      ADD_STAT(demandMissesContextUnknown, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer where the context was not in the backing storage"),
      ADD_STAT(demandMissesPfInflight, statistics::units::Count::get(),
              "On-demand misses to the pattern buffer where the context was not scheduled for insertion"),
              // TODO fix stats
      ADD_STAT(ctxHits, statistics::units::Count::get(),
              "Total number of new patterns allocated in any pattern set"),
      ADD_STAT(ptrnHits, statistics::units::Count::get(),
              "Total number of new patterns allocated in any pattern set"),
      ADD_STAT(patternHits, statistics::units::Count::get(),
              "Number of times any pattern was hit (distribution)"),
      ADD_STAT(patternUseful, statistics::units::Count::get(),
              "Number of times any pattern was useful (distribution)"),
      ADD_STAT(patternSetOccupancy, statistics::units::Count::get(),
              "Number of patterns used in the pattern sets (distribution)"),
      ADD_STAT(patternBufferEvictions, statistics::units::Count::get(),
              "Number of pattern sets evicted from the pattern buffer due to capacity limits"),
      ADD_STAT(backingStorageEvictions, statistics::units::Count::get(),
              "Number of pattern sets evicted from the backing storage due to capacity limits"),
      ADD_STAT(backingStorageInsertions, statistics::units::Count::get(),
              "Number of pattern sets inserted into the backing storage (including replacements)"),
      ADD_STAT(correctOverridesTotal, statistics::units::Count::get(),
              "Number of branches predicted correctly by LLBP (LLBP was provider)"),
      ADD_STAT(correctOverridesIdentical, statistics::units::Count::get(),
              "Number of branches predicted correctly by LLBP, but the base predictor would also be correct (neutral)"),
      ADD_STAT(correctOverridesUnique, statistics::units::Count::get(),
              "Number of branches predicted correctly by LLBP, where the base predictor would be incorrect (good)"),
      ADD_STAT(wrongOverridesTotal, statistics::units::Count::get(),
              "Number of branches predicted wrong by LLBP (LLBP was provider)"),
      ADD_STAT(wrongOverridesIdentical, statistics::units::Count::get(),
              "Number of branches predicted wrong by LLBP, but the base predictor would also be wrong (neutral)"),
      ADD_STAT(wrongOverridesUnique, statistics::units::Count::get(),
              "Number of branches predicted correctly by LLBP, where the base predictor would be correct (bad)"),
      ADD_STAT(squashedOverrides, statistics::units::Count::get(),
              "Number of branches predicted by LLBP, but squashed before the outcome was known"),
      ADD_STAT(profitOrLoss, statistics::units::Count::get(),
              "Net P/L of (unique correct overrides - unique wrong overrides)"),
      ADD_STAT(lightningHitsTotal, statistics::units::Count::get(),
              "Number of branches overridden by lightning predictions"),
      ADD_STAT(lightningHitsRegret, statistics::units::Count::get(),
              "Number of branches (theoretically) overridden by lightning predictions, but turned out incorrect and different from base pred")
              {
                patternHits.init(16).flags(statistics::pdf);
                patternUseful.init(16).flags(statistics::pdf);
                if (parent)
                    patternSetOccupancy.init(parent->patternSetCapacity ? parent->patternSetCapacity + 1 : 16).flags(statistics::pdf);
                else
                    patternSetOccupancy.init(17).flags(statistics::pdf);

                correctOverridesUnique = correctOverridesTotal - correctOverridesIdentical;
                wrongOverridesUnique = wrongOverridesTotal - wrongOverridesIdentical;

                profitOrLoss = correctOverridesUnique - wrongOverridesUnique;
              }


void
LLBP_TAGE_64KB::handleAllocAndUReset(bool alloc, bool taken, TAGEBase::BranchInfo* bi, int nrand)
{
    alloc_banks.clear();

    // If LLBP has overridden the base predictor and a misprediction occured
    // we need to let the base predictor know which length of the history
    // has been matched.
    bool modified = false;
    if (parent->curUpdateBi->overridden) {
        // If LLBP was provider we allocate if the prediction was wrong
        // and the history length is shorter than the maximum.
        alloc = (parent->curUpdateBi->llbp_pred != taken) && (parent->curUpdateBi->index < nHistoryTables);
        bi->hitBank = parent->curUpdateBi->index;
        modified = true;
    }
    // Do the actual allocation
    TAGE_SC_L_TAGE_64KB::handleAllocAndUReset(alloc, taken, bi, nrand);

    // Afterwards, reset the override otherwise the base predictor
    // will update an incorrect entry
    if (modified) {
        bi->hitBank = 0;
    }
}

int
LLBP_TAGE_64KB::allocateEntry(int bank, TAGEBase::BranchInfo* bi, bool taken)
{
    auto r = TAGE_SC_L_TAGE_64KB::allocateEntry(bank, bi, taken);

    // If the allocation was successful, record the table bank such that
    // LLBP can allocate a new pattern with the same history length.
    if (r > 0) {
        alloc_banks.push_back(bank);
    }
    return r;
}

void
LLBP_TAGE_64KB::handleTAGEUpdate(Addr pc, bool taken, TAGEBase::BranchInfo* bi)
{
    // Update the usefulness
    if (parent->curUpdateBi->index > 0) {
        // If TAGE was provider, it was correct and
        // LLBP was incorrect this prediction was useful.
        if ((!parent->curUpdateBi->overridden) && (bi->longestMatchPred == taken) && (parent->curUpdateBi->llbp_pred != taken)) {
            if (bi->hitBank > 0) {
                if(gtable[bi->hitBank][bi->hitBankIndex].u < ((1 << tagTableUBits) -1)) {
                    gtable[bi->hitBank][bi->hitBankIndex].u++;
                }
            }
        }
    }
    // Only update the providing component if LLBP has overridden than
    // don't update TAGE. The BIM might be updated if the LLBP override
    // is weak confidence.
    if (parent->curUpdateBi->overridden) { 
        // if (parent->overridePred != taken) {

        // }
        return;
    }
    // If not overridden do the normal TAGE update
    TAGE_SC_L_TAGE_64KB::handleTAGEUpdate(pc, taken, bi);
}

bool
LLBP_TAGE_64KB::isUseful(bool taken, TAGEBase::BranchInfo* bi) const
{
    // If LLBP overrides we do the usefulness update in `handleTAGEUpdate`
    return (parent->curUpdateBi->index > 0) ? false
           : TAGE_SC_L_TAGE_64KB::isUseful(taken, bi);
}

bool
LLBP_TAGE_64KB::isNotUseful(bool taken, TAGEBase::BranchInfo* bi) const
{
    return (parent->curUpdateBi->index > 0) ? false
           : TAGE_SC_L_TAGE_64KB::isNotUseful(taken, bi);
}

} // namespace branch_prediction
} // namespace gem5
