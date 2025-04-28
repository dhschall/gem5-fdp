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
#include "base/types.hh"
#include "cpu/pred/ltage.hh"
#include "params/LLBP.hh"

namespace gem5
{

namespace branch_prediction
{

class LLBP : public LTAGE
{
  public:
    LLBP(const LLBPParams &params);

    void squash(ThreadID tid, void * &bp_history) override;
    void update(ThreadID tid, Addr pc, bool taken,
                void * &bp_history, bool squashed,
                const StaticInstPtr & inst, Addr target) override;

    void init() override;

    protected:

    struct LLBPBranchInfo : public LTageBranchInfo
    {
        bool overridden;
        bool pred_taken;
        LLBPBranchInfo(TAGEBase &tage, LoopPredictor &lp,
                        Addr pc, bool conditional)
          : LTageBranchInfo(tage, lp, pc, conditional),
          overridden(false)
        {}

        virtual ~LLBPBranchInfo()
        {}
    };

    Prediction predict(
        ThreadID tid, Addr branch_pc, bool cond_branch, void* &b) override;

    struct Pattern
    {
        int tag;
        // direction & whether to replace if low on space in a context
        int8_t counter;
    };

    struct Context
    {
        std::vector<Pattern> patterns;
        uint8_t replace; // whether to replace context in the storage
    };

    std::unordered_map<uint64_t, Context> storage;
    std::vector<std::unordered_set<uint64_t>> storagePriority;
    std::deque<uint64_t> patternBuffer;

    int contextCapacity;
    int patternBufferCapacity;
    int storageCapacity;
    int ctxCounterBits;
    int ptnCounterBits;
    int overrides = 0;
    int llbp_hits = 0;

    int8_t normalize(int8_t counter);
    void storageUpdate(
        ThreadID tid, Addr pc, uint64_t cid, bool taken, LLBPBranchInfo* bi);
    void storageInvalidate();
    int findBestPattern(ThreadID tid, Addr pc, Context& ctx);
    int findVictimPattern(int min, Context& ctx);
    uint64_t findVictimContext();


    /* From LLBP Source Code */

    class RCR
    {
        const int maxwindow = 120;

        uint64_t calcHash(std::list<uint64_t> &vec, int n,
            int start=0, int shift=0);

        // The context tag width
        const int CTWidth;

        // A list of previouly taken branches
        std::list<uint64_t> bb[10];

        // We compute the context ID and prefetch context ID
        // only when the content of the RCR changes.
        struct
        {
            uint64_t ccid = 0;
            uint64_t pcid = 0;
        } ctxs;

        int branchCount = 0;

    public:

        // The hash constants
        const int T, W, D, S;

        RCR(int _T, int _W, int _D, int _shift, int _CTWidth);

        // Push a new branch into the RCR.
        bool update(Addr pc, const StaticInstPtr & inst, bool taken);


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
