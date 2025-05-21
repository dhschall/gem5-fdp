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

#ifndef __CPU_PRED_LLBP_HH__
#define __CPU_PRED_LLBP_HH__

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/cache/associative_cache.hh"
#include "base/cache/cache_entry.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/pred/ltage.hh"
#include "params/LLBP.hh"

namespace gem5
{

namespace branch_prediction
{

class LLBP : public ConditionalPredictor
{
  public:
    LLBP(const LLBPParams &params);

    Prediction lookup(ThreadID tid, Addr pc, void * &bp_history) override;

    void squash(ThreadID tid, void * &bp_history) override;
    void update(ThreadID tid, Addr pc, bool taken,
                void * &bp_history, bool squashed,
                const StaticInstPtr & inst, Addr target) override;

    void init() override;
    void branchPlaceholder(ThreadID tid, Addr pc,
                        bool uncond, void * &bpHistory) override;

    void updateHistories(ThreadID tid, Addr pc, bool uncond,
                         bool taken, Addr target,
                         const StaticInstPtr &inst,
                         void * &bp_history) override;
  protected:

    LTAGE* base;
    
    Cycles calculateRemainingLatency(Cycles insertTime);

    Prediction predict(ThreadID tid, Addr pc,
        bool cond_branch, void * &bp_history);

    struct LLBPBranchInfo
    {
        bool overridden;
        bool llbp_pred;
        bool base_pred;
        bool avenged;
        Addr pc;
        int index;
        bool conditional;
        std::list<uint64_t> rcrBackup;
        uint64_t cid;
        void* ltage_bi;
        
        LLBPBranchInfo(Addr pc, bool conditional)
          : overridden(false),
            avenged(false),
            pc(pc),
            index(-1),
            conditional(conditional),
            cid(-1),
            ltage_bi(nullptr)
        {}

        bool getPrediction() {
            return overridden ? llbp_pred : base_pred;
        }

        ~LLBPBranchInfo()
        {}
    };


    struct Pattern
    {
        //* hysteresis counter: > 0 = taken, < 0 = not taken
        int8_t counter;
        int visited = 0;
    };

    struct Context
    {
        std::unordered_map<int, Pattern> patterns;
        /** Confidence counter of the context (guides replacement) */
        uint8_t confidence;
    };

    struct PatternBufferEntry
    {
        uint64_t cid;
        Cycles insertTime;
    };

    std::unordered_map<uint64_t, Context> backingStorage;
    std::unordered_map<uint64_t, Cycles> patternBuffer;
    std::deque<uint64_t> patternBufferQueue;

    int patternBufferCapacity;
    int storageCapacity;
    int ctxCounterBits;
    int ptnCounterBits;

    int calculateTag(int tageTag, int tageBank) const
    {
        return (tageTag << 4) + tageBank;
    }

    Cycles backingStorageLatency;

    int8_t absPredCounter(int8_t counter);
    void storageUpdate(ThreadID tid, Addr pc, bool taken, LLBPBranchInfo* bi);
    void storageInvalidate();
    int findBestPattern(Context &ctx, TAGEBase::BranchInfo *bi);
    int findVictimPattern(int min, Context& ctx);
    uint64_t findVictimContext();

    struct LLBPStats : public statistics::Group
    {
        LLBPStats(LLBP *llbp);

        statistics::Scalar prefetchesIssued;
        statistics::Scalar baseHitsTotal;
        statistics::Scalar demandHitsTotal;
        statistics::Scalar demandHitsOverride;
        statistics::Scalar demandHitsNoOverride;
        statistics::Scalar demandMissesTotal;
        statistics::Scalar demandMissesNoPattern;
        statistics::Scalar demandMissesNoPrefetch;
        statistics::Scalar demandMissesCold;
        statistics::Scalar allocationsTotal;
        statistics::Vector revisits;
        statistics::Scalar patternBufferEvictions;
        statistics::Scalar backingStorageEvictions;
        statistics::Scalar backingStorageInsertions;
        statistics::Scalar correctOverridesTotal;
        statistics::Scalar correctOverridesIdentical;
        statistics::Scalar wrongOverridesTotal;
        statistics::Scalar wrongOverridesIdentical;
        statistics::Scalar squashedOverrides;
    } stats;


    /* From LLBP Source Code */

    class RCR
    {
    public:
        const int maxwindow = 120;

        uint64_t calcHash(int n,
            int start=0, int shift=0);

        // The context tag width
        const int tagWidthBits;

        // A list of previouly taken branches
        std::list<uint64_t> bb;

        // We compute the context ID and prefetch context ID
        // only when the content of the RCR changes.
        struct
        {
            uint64_t ccid = 0;
            uint64_t pcid = 0;
        } ctxs;


        // The hash constants
        const int T, W, D, S;

        RCR(int _T, int _W, int _D, int _shift, int _CTWidth);

        // Push a new branch into the RCR.
        bool update(Addr pc, const StaticInstPtr & inst, bool taken);

        // Save the RCR state into a list
        void backup(std::list<uint64_t> &vec);

        // Restore the RCR state from a list
        void restore(std::list<uint64_t> &vec);

        /**
          * Computes the modulo of a number val with respect to 2^exp
          * i.e. val % 2^exp
          *
          * @param val The value to be wrapped
          * @param exp The size as exponent of 2
          * @return The wrapped value
        */
        inline static uint64_t moduloTwoExp(uint64_t val, int exp) {
            return val & ((1 << exp) - 1);
        }

        // Get the current context ID
        uint64_t getCCID();

        // Get the prefetch context ID
        uint64_t getPCID();
    } rcr;
};

} // namespace branch_prediction
} // namespace gem5

 #endif // __CPU_PRED_LLBP_HH__
