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
 * Derived from the 64KB implementation in tage_sc_l_64KB.{hh,cc}.
 * Table sizes and history lengths have been scaled to fit a 128KB budget;
 * the architectural structure (three ordinal local histories, p/s/t/im GEHL
 * banks, bank-interleaved TAGE) is identical to the 64KB variant.
 */

#ifndef __CPU_PRED_TAGE_SC_L_128KB_HH__
#define __CPU_PRED_TAGE_SC_L_128KB_HH__

#include "cpu/pred/tage_sc_l.hh"
#include "params/TAGE_SC_L_128KB.hh"
#include "params/TAGE_SC_L_128KB_StatisticalCorrector.hh"
#include "params/TAGE_SC_L_TAGE_128KB.hh"

namespace gem5
{

namespace branch_prediction
{

// ---------------------------------------------------------------------------
// TAGE component
// ---------------------------------------------------------------------------

class TAGE_SC_L_TAGE_128KB : public TAGE_SC_L_TAGE
{
  public:
    TAGE_SC_L_TAGE_128KB(const TAGE_SC_L_TAGE_128KBParams &p)
        : TAGE_SC_L_TAGE(p)
    {}

    // Index extension: no extra folding needed at this size (same as 64KB).
    int gindex_ext(int index, int bank) const override;

    // Tag computation: XOR of compressed history components, no PC shift.
    uint16_t gtag(ThreadID tid, Addr pc, int bank) const override;

    // Allocation and useful-bit reset logic (bank-interleaved, 2-way assoc).
    void handleAllocAndUReset(
        bool alloc, bool taken, TAGEBase::BranchInfo *bi, int nrand) override;

    // Counter and useful-bit update after resolution.
    void handleTAGEUpdate(
        Addr branch_pc, bool taken, TAGEBase::BranchInfo *bi) override;
};

// ---------------------------------------------------------------------------
// Statistical Corrector
// ---------------------------------------------------------------------------

class TAGE_SC_L_128KB_StatisticalCorrector : public StatisticalCorrector
{
    // ---- Sizing for the two extra local history tables --------------------
    const unsigned numEntriesSecondLocalHistories;
    const unsigned numEntriesThirdLocalHistories;

    // ---- Global branch history variation GEHL (P tables) -----------------
    const unsigned pnb;
    const unsigned logPnb;
    std::vector<int> pm;
    std::vector<int8_t> *pgehl;
    std::vector<int8_t> wp;

    // ---- Second local history GEHL (S tables) ----------------------------
    const unsigned snb;
    const unsigned logSnb;
    std::vector<int> sm;
    std::vector<int8_t> *sgehl;
    std::vector<int8_t> ws;

    // ---- Third local history GEHL (T tables) -----------------------------
    const unsigned tnb;
    const unsigned logTnb;
    std::vector<int> tm;
    std::vector<int8_t> *tgehl;
    std::vector<int8_t> wt;

    // ---- Second IMLI GEHL (IM tables) ------------------------------------
    const unsigned imnb;
    const unsigned logImnb;
    std::vector<int> imm;
    std::vector<int8_t> *imgehl;
    std::vector<int8_t> wim;

    // ---- Per-thread history state ----------------------------------------
    // Identical layout to SC_64KB_ThreadHistory; only the table capacities
    // (set by parameters) differ.
    struct SC_128KB_ThreadHistory : public SCThreadHistory
    {
        SC_128KB_ThreadHistory(unsigned instShiftAmt)
            : SCThreadHistory(instShiftAmt) {}

        // IMLI-indexed history vector; sized to 2^im[0] entries.
        std::vector<int64_t> imHist;
    };

    SCThreadHistory *makeThreadHistory() override;

  public:
    TAGE_SC_L_128KB_StatisticalCorrector(
        const TAGE_SC_L_128KB_StatisticalCorrectorParams &p);

    // Bias bank index: same formula as 64KB; scales via logBias parameter.
    unsigned getIndBiasBank(Addr branch_pc, BranchInfo *bi, int hitBank,
                            int altBank) const override;

    // Accumulate predictions from all GEHL banks into lsum.
    int gPredictions(ThreadID tid, Addr branch_pc, BranchInfo *bi,
                     int &lsum) override;

    // Log-size subtraction used when hashing indices into GEHL tables.
    // Returns 1 for the two highest-indexed banks, 0 otherwise.
    int gIndexLogsSubstr(int nbr, int i) override;

    // Non-speculative update of SC-specific histories on each conditional
    // branch resolution.
    void scHistoryUpdate(Addr branch_pc, const StaticInstPtr &inst, bool taken,
                         Addr target, int64_t phist) override;

    // Snapshot SC histories into BranchInfo for later recovery.
    void scRecordHistState(Addr branch_pc, BranchInfo *bi) override;

    // Restore SC histories from a previously saved snapshot.
    bool scRestoreHistState(BranchInfo *bi) override;

    // Update all GEHL weight tables after branch resolution.
    void gUpdates(ThreadID tid, Addr pc, bool taken, BranchInfo *bi) override;
};

// ---------------------------------------------------------------------------
// Top-level predictor
// ---------------------------------------------------------------------------

class TAGE_SC_L_128KB : public TAGE_SC_L
{
  public:
    TAGE_SC_L_128KB(const TAGE_SC_L_128KBParams &params);
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_TAGE_SC_L_128KB_HH__
