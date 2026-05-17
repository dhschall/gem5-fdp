/*
 * Copyright (c) 2018 Metempsy Technology Consulting
 * All rights reserved.
 *
 * Copyright (c) 2006 INRIA (Institut National de Recherche en
 * Informatique et en Automatique  / French National Research Institute
 * for Computer Science and Applied Mathematics)
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: André Seznec, Pau Cabre, Javier Bueno
 *
 */

/*
 * 128KB TAGE-SC-L branch predictor (devised by Andre Seznec)
 *
 * Derived from the 64KB implementation in tage_sc_l_64KB.cc.
 * All logic is structurally identical; only class names and the Python-side
 * parameter values differ.
 */

#include "cpu/pred/tage_sc_l_128KB.hh"

namespace gem5
{

namespace branch_prediction
{

// ===========================================================================
// TAGE_SC_L_128KB_StatisticalCorrector
// ===========================================================================

TAGE_SC_L_128KB_StatisticalCorrector::TAGE_SC_L_128KB_StatisticalCorrector(
    const TAGE_SC_L_128KB_StatisticalCorrectorParams &p)
  : StatisticalCorrector(p),
    numEntriesSecondLocalHistories(p.numEntriesSecondLocalHistories),
    numEntriesThirdLocalHistories(p.numEntriesThirdLocalHistories),
    pnb(p.pnb),
    logPnb(p.logPnb),
    pm(p.pm),
    snb(p.snb),
    logSnb(p.logSnb),
    sm(p.sm),
    tnb(p.tnb),
    logTnb(p.logTnb),
    tm(p.tm),
    imnb(p.imnb),
    logImnb(p.logImnb),
    imm(p.imm)
{
    // Initialise GEHL tables for the four banks unique to this SC variant.
    // Initial weight values match the 64KB design (7 for most, 0 for IMLI).
    initGEHLTable(pnb,  pm,  pgehl,  logPnb,  wp,  7);
    initGEHLTable(snb,  sm,  sgehl,  logSnb,  ws,  7);
    initGEHLTable(tnb,  tm,  tgehl,  logTnb,  wt,  7);
    initGEHLTable(imnb, imm, imgehl, logImnb, wim, 0);
}

StatisticalCorrector::SCThreadHistory *
TAGE_SC_L_128KB_StatisticalCorrector::makeThreadHistory()
{
    SC_128KB_ThreadHistory *sh =
        new SC_128KB_ThreadHistory(instShiftAmt);

    // Three ordinal local histories, matching the 64KB structure.
    sh->setNumOrdinalHistories(3);
    sh->initLocalHistory(1, numEntriesFirstLocalHistories, 2);
    sh->initLocalHistory(2, numEntriesSecondLocalHistories, 5);
    sh->initLocalHistory(3, numEntriesThirdLocalHistories, logTnb);

    // IMLI history vector: one entry per possible imliCount value.
    sh->imHist.resize(1 << im[0]);
    return sh;
}

unsigned
TAGE_SC_L_128KB_StatisticalCorrector::getIndBiasBank(Addr branch_pc,
                                                     BranchInfo *bi,
                                                     int hitBank,
                                                     int altBank) const
{
    // Identical formula to 64KB; scaling comes from the logBias parameter.
    Addr shifted_pc = branch_pc >> instShiftAmt;
    return (bi->predBeforeSC
            + (((hitBank + 1) / 4) << 4)
            + (bi->highConf << 1)
            + (bi->lowConf  << 2)
            + ((altBank != 0) << 3)
            + ((shifted_pc ^ (shifted_pc >> 2)) << 7))
           & ((1 << logBias) - 1);
}

int
TAGE_SC_L_128KB_StatisticalCorrector::gPredictions(ThreadID tid,
                                                   Addr branch_pc,
                                                   BranchInfo *bi,
                                                   int &lsum)
{
    // Backward branch global history
    lsum += gPredict(
        ((branch_pc >> instShiftAmt) << 1) + bi->predBeforeSC,
        bi->bwHist, bwm, bwgehl, bwnb, logBwnb, wbw);

    // Global branch history variation
    lsum += gPredict(
        branch_pc, bi->pHist, pm, pgehl, pnb, logPnb, wp);

    // First local history
    lsum += gPredict(
        branch_pc, bi->localHistories[1], lm, lgehl, lnb, logLnb, wl);

    // Second local history
    lsum += gPredict(
        branch_pc, bi->localHistories[2], sm, sgehl, snb, logSnb, ws);

    // Third local history
    lsum += gPredict(
        branch_pc, bi->localHistories[3], tm, tgehl, tnb, logTnb, wt);

    // Second IMLI history
    lsum += gPredict(
        branch_pc, bi->imHist, imm, imgehl, imnb, logImnb, wim);

    // IMLI counter
    lsum += gPredict(
        branch_pc, bi->imliCount, im, igehl, inb, logInb, wi);

    // Dynamic update threshold.
    // The weight-sign terms count how many banks agree with the current
    // prediction; this guards against over-confident updates.
    int thres =
        (updateThreshold >> 3)
        + pUpdateThreshold[getIndUpd(branch_pc)]
        + 12 * ((wb [getIndUpds(branch_pc)] >= 0)
              + (wp [getIndUpds(branch_pc)] >= 0)
              + (ws [getIndUpds(branch_pc)] >= 0)
              + (wt [getIndUpds(branch_pc)] >= 0)
              + (wl [getIndUpds(branch_pc)] >= 0)
              + (wbw[getIndUpds(branch_pc)] >= 0)
              + (wi [getIndUpds(branch_pc)] >= 0));

    return thres;
}

int
TAGE_SC_L_128KB_StatisticalCorrector::gIndexLogsSubstr(int nbr, int i)
{
    // Return 1 for the two highest-indexed banks to reduce their effective
    // table size by one bit, consistent with the 64KB behaviour.
    return (i >= (nbr - 2)) ? 1 : 0;
}

void
TAGE_SC_L_128KB_StatisticalCorrector::scHistoryUpdate(
    Addr branch_pc, const StaticInstPtr &inst, bool taken,
    Addr target, int64_t phist)
{
    int brtype = inst->isDirectCtrl() ? 0 : 2;
    if (!inst->isUncondCtrl()) {
        ++brtype;
    }

    // Only conditional branches update the SC-private histories.
    if (brtype & 1) {
        SC_128KB_ThreadHistory *sh =
            static_cast<SC_128KB_ThreadHistory *>(scHistory);

        int64_t imliCount = sh->imliCount;
        sh->imHist[imliCount] = (sh->imHist[imliCount] << 1) + taken;

        // Update second and third local histories for conditional branches.
        sh->updateLocalHistory(2, branch_pc, taken, branch_pc & 15);
        sh->updateLocalHistory(3, branch_pc, taken);
    }

    // Delegate BW history and base histories to the parent.
    StatisticalCorrector::scHistoryUpdate(branch_pc, inst, taken,
                                          target, phist);
}

void
TAGE_SC_L_128KB_StatisticalCorrector::scRecordHistState(Addr branch_pc,
                                                        BranchInfo *bi)
{
    StatisticalCorrector::scRecordHistState(branch_pc, bi);

    auto sh = static_cast<SC_128KB_ThreadHistory *>(scHistory);

    // Snapshot the extra histories that are not tracked by the base class.
    bi->imHist           = sh->imHist[sh->imliCount];
    bi->localHistories[2] = sh->getLocalHistory(2, branch_pc);
    bi->localHistories[3] = sh->getLocalHistory(3, branch_pc);
}

bool
TAGE_SC_L_128KB_StatisticalCorrector::scRestoreHistState(BranchInfo *bi)
{
    if (!StatisticalCorrector::scRestoreHistState(bi)) {
        return false;
    }

    auto sh = static_cast<SC_128KB_ThreadHistory *>(scHistory);

    // Restore the extra histories from the snapshot.
    sh->imHist[sh->imliCount] = bi->imHist;
    sh->setLocalHistory(2, bi->pc, bi->localHistories[2]);
    sh->setLocalHistory(3, bi->pc, bi->localHistories[3]);
    return true;
}

void
TAGE_SC_L_128KB_StatisticalCorrector::gUpdates(ThreadID tid, Addr pc,
                                               bool taken, BranchInfo *bi)
{
    gUpdate((pc << 1) + bi->predBeforeSC, taken, bi->bwHist,
            bwm, bwgehl, bwnb, logBwnb, wbw, bi);

    gUpdate(pc, taken, bi->pHist,
            pm, pgehl, pnb, logPnb, wp, bi);

    gUpdate(pc, taken, bi->localHistories[1],
            lm, lgehl, lnb, logLnb, wl, bi);

    gUpdate(pc, taken, bi->localHistories[2],
            sm, sgehl, snb, logSnb, ws, bi);

    gUpdate(pc, taken, bi->localHistories[3],
            tm, tgehl, tnb, logTnb, wt, bi);

    gUpdate(pc, taken, bi->imHist,
            imm, imgehl, imnb, logImnb, wim, bi);

    gUpdate(pc, taken, bi->imliCount,
            im, igehl, inb, logInb, wi, bi);
}

// ===========================================================================
// TAGE_SC_L_TAGE_128KB
// ===========================================================================

int
TAGE_SC_L_TAGE_128KB::gindex_ext(int index, int bank) const
{
    // No extra index extension required at 128KB scale (same as 64KB).
    return index;
}

uint16_t
TAGE_SC_L_TAGE_128KB::gtag(ThreadID tid, Addr pc, int bank) const
{
    // Tag = XOR of two compressed history components (with a 1-bit left-shift
    // on the second), XOR-ed with the unshifted PC.  Identical to 64KB.
    Addr shifted_pc = pc >> instShiftAmt;
    int tag = shifted_pc
            ^ threadHistory[tid].computeTags[0][bank].comp
            ^ (threadHistory[tid].computeTags[1][bank].comp << 1);

    return (tag & ((1ULL << tagTableTagWidths[bank]) - 1));
}

void
TAGE_SC_L_TAGE_128KB::handleAllocAndUReset(bool alloc, bool taken,
                                           TAGEBase::BranchInfo *bi,
                                           int nrand)
{
    if (!alloc) {
        return;
    }

    int penalty      = 0;
    int numAllocated = 0;
    bool maxAllocReached = false;

    // Walk tagged banks in steps of 2 (bank interleaving).
    for (int I = calcDep(bi); I < nHistoryTables; I += 2) {
        // Try both the even (I) and odd (I^1) partner of each bank pair.
        for (int j = 0; j < 2; ++j) {
            int i = ((j == 0) ? I : (I ^ 1)) + 1;
            if (noSkip[i]) {
                if (gtable[i][bi->tableIndices[i]].u == 0) {
                    int8_t ctr = gtable[i][bi->tableIndices[i]].ctr;
                    if (abs(2 * ctr + 1) <= 3) {
                        // Entry is weakly predicted: allocate here.
                        gtable[i][bi->tableIndices[i]].tag = bi->tableTags[i];
                        gtable[i][bi->tableIndices[i]].ctr = taken ? 0 : -1;
                        numAllocated++;
                        maxAllocReached = (numAllocated == maxNumAlloc);
                        I += 2;  // skip the next pair; we found a home
                        break;
                    } else {
                        // Occupied but useful; decay the counter toward zero.
                        if (gtable[i][bi->tableIndices[i]].ctr > 0) {
                            gtable[i][bi->tableIndices[i]].ctr--;
                        } else {
                            gtable[i][bi->tableIndices[i]].ctr++;
                        }
                    }
                } else {
                    penalty++;
                }
            }
        }
        if (maxAllocReached) {
            break;
        }
    }

    // Adjust the useful-bit reset counter: penalty for failed allocations,
    // reward for successful ones.
    tCounter += (penalty - 2 * numAllocated);

    handleUReset();
}

void
TAGE_SC_L_TAGE_128KB::handleTAGEUpdate(Addr branch_pc, bool taken,
                                       TAGEBase::BranchInfo *bi)
{
    if (bi->hitBank > 0) {
        // When the hit-bank counter is weakly confident and was wrong,
        // also update the alt-bank counter (protection mechanism).
        if (abs(2 * gtable[bi->hitBank][bi->hitBankIndex].ctr + 1) == 1) {
            if (bi->longestMatchPred != taken) {
                if (bi->altBank > 0) {
                    ctrUpdate(gtable[bi->altBank][bi->altBankIndex].ctr,
                              taken, tagTableCounterBits);
                }
                if (bi->altBank == 0) {
                    baseUpdate(branch_pc, taken, bi);
                }
            }
        }

        // Update the hit-bank counter.
        ctrUpdate(gtable[bi->hitBank][bi->hitBankIndex].ctr,
                  taken, tagTableCounterBits);

        // If the counter crossed zero it cannot have been useful; clear u.
        if (abs(2 * gtable[bi->hitBank][bi->hitBankIndex].ctr + 1) == 1) {
            gtable[bi->hitBank][bi->hitBankIndex].u = 0;
        }

        // Decay the useful bit of the hit-bank entry when the alt-bank is
        // already strongly confident in the correct direction.
        if (bi->altTaken == taken) {
            if (bi->altBank > 0) {
                int8_t ctr = gtable[bi->altBank][bi->altBankIndex].ctr;
                if (abs(2 * ctr + 1) == 7) {
                    if (gtable[bi->hitBank][bi->hitBankIndex].u == 1) {
                        if (bi->longestMatchPred == taken) {
                            gtable[bi->hitBank][bi->hitBankIndex].u = 0;
                        }
                    }
                }
            }
        }
    } else {
        baseUpdate(branch_pc, taken, bi);
    }

    // Increment useful bit when the longest-match and alt predictions
    // disagreed and the longest-match was correct.
    if ((bi->longestMatchPred != bi->altTaken) &&
        (bi->longestMatchPred == taken) &&
        (gtable[bi->hitBank][bi->hitBankIndex].u <
         (1 << tagTableUBits) - 1))
    {
        gtable[bi->hitBank][bi->hitBankIndex].u++;
    }
}

// ===========================================================================
// TAGE_SC_L_128KB
// ===========================================================================

TAGE_SC_L_128KB::TAGE_SC_L_128KB(const TAGE_SC_L_128KBParams &params)
  : TAGE_SC_L(params)
{
}

} // namespace branch_prediction
} // namespace gem5
