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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/cache/associative_cache.hh"
#include "base/cache/cache_entry.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/pred/tage_sc_l.hh"
#include "cpu/pred/tage_sc_l_64KB.hh"
#include "params/LLBP.hh"
#include "params/LLBP_TAGE_64KB.hh"

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

    TAGE_SC_L* base;

    int backingStorageCapacity;
    const Cycles backingStorageLatency;

    int patternSetCapacity;
    int patternSetAssoc;
    int patternSetBankBits;

    int contextCounterWidth;
    int patternCounterWidth;

    bool lightningPredEnabled;
    int lightningPredCutoff;
    struct LLBPStats : public statistics::Group
    {
        LLBPStats(LLBP *llbp);

        void preDumpStats() override {
            if (!parent) return;
            for(auto& ctx : parent->backingStorage) {
                ctx.second.patterns.commitStats();
            }
        }

        LLBP* parent;

        statistics::Scalar allocationsTotal;
        statistics::Scalar prefetchesIssued;
        statistics::Scalar baseHitsTotal;
        statistics::Scalar demandHitsTotal;
        statistics::Scalar demandHitsOverride;
        statistics::Scalar demandHitsNoOverride;
        statistics::Scalar demandMissesTotal;
        statistics::Scalar demandMissesPatternMiss;
        statistics::Scalar demandMissesContextTooLate;
        statistics::Scalar demandMissesContextNotPrefetched;
        statistics::Scalar demandMissesContextUnknown;
        statistics::Scalar demandMissesPfInflight;
        statistics::Scalar ctxHits;
        statistics::Scalar ptrnHits;

        statistics::SparseHistogram patternHits;
        statistics::SparseHistogram patternUseful;
        statistics::Histogram patternSetOccupancy;
        statistics::Scalar patternBufferEvictions;
        statistics::Scalar backingStorageEvictions;
        statistics::Scalar backingStorageInsertions;
        statistics::Scalar correctOverridesTotal;
        statistics::Scalar correctOverridesIdentical;
        statistics::Formula correctOverridesUnique;
        statistics::Scalar wrongOverridesTotal;
        statistics::Scalar wrongOverridesIdentical;
        statistics::Formula wrongOverridesUnique;
        statistics::Scalar squashedOverrides;
        statistics::Formula profitOrLoss;
        statistics::Scalar lightningHitsTotal;
        statistics::Scalar lightningHitsRegret;
    } stats;

    Cycles calculateRemainingLatency(Cycles insertTime);

    Prediction predict(ThreadID tid, Addr pc,
        bool cond_branch, void * &bp_history);

    struct LLBPBranchInfo
    {
        bool overridden;
        bool llbp_pred;
        bool base_pred;
        bool lightningTarget;
        Addr pc;
        int index; // TODO: rename this to hitIndex of llbpHit
        uint64_t key;
        bool conditional;
        std::list<uint64_t> rcrBackup;
        uint64_t cid;
        void* ltage_bi;

        LLBPBranchInfo(Addr pc, bool conditional)
          : overridden(false),
            lightningTarget(false),
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
        uint64_t tag = 0;
        int8_t counter = 0;
        int hit = 0;
        int useful = 0;
        bool valid = false;
    };

    class PatternSet {
        typedef typename std::vector<Pattern> set_t;
      public:
        PatternSet(
            int numEntries,
            int associativity,
            int bankBits,
            LLBPStats& stats
        ):  bankBits(bankBits), // TODO: Remove
            associativity(associativity),
            numSets(numEntries / associativity),
            setMask(numSets - 1),
            setSize(numEntries / numSets),
            unbounded(false),
            occupancy(0),
            stats(stats)
        {
            assert(numEntries % setSize == 0);
            assert(associativity * numSets == numEntries);
            // this->numSets = numEntries / setSize;
            sets.resize(numSets);
            for (auto& set : sets) {
                set.resize(setSize);
            }
        }

        PatternSet(int bankBits, LLBPStats& stats)
        :   bankBits(bankBits),
            associativity(1),
            numSets(1),
            setMask(0),
            setSize(0),
            unbounded(true),
            occupancy(0),
            stats(stats)
        {
        }

        ~PatternSet() {
            commitStats();
        }

        // key_t index(const key_t& key) { return key & _set_mask; }

        // set_t& getSet(const key_t& key) {
        //     return _cache[index(key)];
        // }

        
        // int getBank(uint64_t key) {
        //     // FIX: THIS IS BOGUS!
        //     return bitmaskLowerN(bankBits) & key;
        // }

        set_t& getSet(uint64_t key) {
            int idx = int(key & setMask);
            assert(idx < sets.size());
            return sets[idx];
        }

        // int getID(uint64_t key) {
        //     uint64_t bank = getBank(key);
        //     return bank / setSize;
        // }

        Pattern* getEntry(uint64_t key) {
            if (unbounded) {
                Pattern& res = unboundedSet[key];
                if (!res.valid) return nullptr;
                return &res;
            }
            set_t& set = getSet(key);
            Pattern* result = findPatternInSet(key, set);
            return result;
        }

        void insertEntry(uint64_t key, bool taken) {
            if (unbounded) {
                Pattern& tgt = unboundedSet[key];
                tgt.tag = key;
                tgt.counter = taken ? 0 : -1;
                tgt.hit = 0;
                tgt.useful = 0;
                tgt.valid = true;
            } else {
                set_t& set = getSet(key);
                Pattern& victim = findVictimPattern(set);
                if (victim.valid) {
                    stats.patternUseful.sample(victim.useful);
                    stats.patternHits.sample(victim.hit);
                }
                victim.tag = key;
                victim.counter = taken ? 0 : -1;
                victim.hit = 0;
                victim.useful = 0;
                victim.valid = true;
            }

            saturatingAdd(occupancy, associativity * numSets);
        }

        void wasUseful(uint64_t key) {
            Pattern* p = getEntry(key);
            if (p) {
                p->useful++;
            }
        }

        void wasHit(uint64_t key) {
            Pattern* p = getEntry(key);
            if (p) {
                p->hit++;
            }
        }

        void commitStats() {
            if (unbounded) {
                stats.patternSetOccupancy.sample(unboundedSet.size());
            } else {
                stats.patternSetOccupancy.sample(occupancy);
            }
            for (auto& set: sets) {
                for (auto& pat: set) {
                    if (pat.valid) {
                        stats.patternUseful.sample(pat.useful);
                        stats.patternHits.sample(pat.hit);
                    }
                }
            }
        }

        static int absConfidence(int8_t ctr) {
            if (ctr < 0) {
                return abs(ctr) - 1;
            }
            return ctr;
        }

        static uint64_t bitmaskLowerN(int n) { // TODO unused
            return (1 << n) - 1;
        }

        static void saturatingSub(int& n) {
            if (n > 0) {
                --n;
            }
        }

        static void saturatingAdd(int& n, int max) {
            if (n < max) {
                ++n;
            }
        }

      private:

        Pattern* findPatternInSet(uint64_t key, set_t& set) {
            auto result = std::find_if(set.begin(), set.end(), [key](Pattern& pat) {
                return pat.tag == key && pat.valid;
            });

            if (result == set.end())
                return nullptr;

            return &*result;
        }

        Pattern& findVictimPattern(set_t& set) {
            auto firstInvalid = std::find_if(set.begin(), set.end(), [](Pattern& e) {
                return !e.valid;
            });

            if (firstInvalid != set.end())
                return *firstInvalid;

            auto result = std::min_element(set.begin(), set.end(), [&](const Pattern& a, const Pattern& b) {
                return absConfidence(a.counter) < absConfidence(b.counter);
            });

            return *result;
        }

        const int bankBits;
        const int associativity;
        const int numSets;
        const uint64_t setMask;
        const int setSize;
        const bool unbounded;
        int occupancy;

        LLBPStats& stats;
        std::unordered_map<uint64_t, Pattern> unboundedSet;
        std::vector<set_t> sets;
    };

    class Context
    {
      public:
        PatternSet patterns;
        /** Confidence counter of the context (guides replacement) */
        uint8_t confidence;

        Context(PatternSet patterns): patterns(patterns), confidence(0) {}
    };


    // TODO: Make this a set-associative cache
    // Use the underlying structure (template) that is used for the pattern set.
    typedef std::unordered_map<uint64_t, Context> BackingStorage;

    BackingStorage backingStorage;

    struct PatternBufferEntry {
        uint64_t cid;
        Cycles insertTime;
        Cycles lastUsed;
        bool valid = false;
    };


    class PatternBuffer {
      public:
        PatternBuffer(
            int numEntries,
            int setSize,
            BackingStorage& backingStorage,
            statistics::Scalar& patternBufferEvictions
        )
          : setSize(setSize),
            backingStorage(backingStorage),
            patternBufferEvictions(patternBufferEvictions)
        {
            assert(numEntries % setSize == 0);
            this->numSets = numEntries / setSize;

            sets.resize(numSets);
            for (auto& set : sets) {
                set.resize(setSize);
            }
        }

        void insert(uint64_t cid, Cycles now) {
            auto& set = getSet(cid);
            if (!backingStorage.count(cid))
                return;
            PatternBufferEntry& victim = findVictim(set);
            if (victim.valid) {
                ++patternBufferEvictions;
            }
            victim.cid = cid;
            victim.insertTime = now;
            victim.lastUsed = now;
            victim.valid = true;
        }

        PatternBufferEntry* get(uint64_t cid) {
            auto& set = getSet(cid);
            return findEntry(cid, set);
        }

        void clearInFlight(Cycles now, Cycles latency) {
            for (auto& set: sets) {
                for (auto& e: set) {
                    Cycles passedTime = (now - e.insertTime);
                    if (passedTime < latency) {
                        e.valid = false;
                    }
                }
            }
        }

        std::vector<PatternBufferEntry>& getSet(uint64_t cid) {
            return sets[cid % numSets];
        }

      private:
        PatternBufferEntry* findEntry(uint64_t cid, std::vector<PatternBufferEntry>& set) {
            auto result = std::find_if(set.begin(), set.end(), [cid](PatternBufferEntry& e) {
                return e.cid == cid && e.valid;
            });

            if (result == set.end())
                return nullptr;

            return &*result;
        }

        PatternBufferEntry& findVictim(std::vector<PatternBufferEntry>& set) {
            auto firstInvalid = std::find_if(set.begin(), set.end(), [](PatternBufferEntry& e) {
                return !e.valid;
            });

            if (firstInvalid != set.end())
                return *firstInvalid;

            auto worst = std::min_element(set.begin(), set.end(), [&](const PatternBufferEntry& a, const PatternBufferEntry& b) {
                return a.insertTime < b.insertTime;
            });

            return *worst;
        }
        int numSets;
        int setSize;
        BackingStorage& backingStorage;
        statistics::Scalar& patternBufferEvictions;
        std::vector<std::vector<PatternBufferEntry>> sets;
    } patternBuffer;

    TAGEBase::FoldedHistory fghrT1[40];
    TAGEBase::FoldedHistory fghrT2[40];

    uint64_t KEY[40]; // Key for each history length
    void calculateKeys(Addr pc);

    public:
    // The branch info of the branch that currently gets updated.
    // A bit of a hack to communicate the LLBP prediction information
    // to the base predictor.
    LLBPBranchInfo *curUpdateBi = nullptr; // The branch info of the current update

    protected:
    // TODO: move all parameters together, make them const and add details. Do the same in the python file.
    const int TTWidth; // Tag table width in bits
    const bool optimalPrefetching; // Ignores prefetching into the PB
    // const int patternSetSize; // Number of patterns per context
    // const int patternSetAssociativity; // Associativity of the pattern set
    // const int patternSetSetSize; // Number of sets in the pattern set

    // A map to filter the used history lengths.
    std::vector<int> fltTables;
    int branchCount = 0; // Number of branches executed

    uint64_t calculateKey(TAGEBase::BranchInfo* tageBi, int tageBank, Addr pc);



    int8_t absPredCounter(int8_t counter);
    void storageUpdate(ThreadID tid, Addr pc, bool taken, LLBPBranchInfo* bi);
    void storageInvalidate();
    int findBestPattern(Context &ctx, TAGEBase::BranchInfo *bi, Addr pc);
    int findVictimPattern(int min, Context& ctx);
    uint64_t findVictimContext();



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
            return val & ((1 << (uint64_t) exp) - 1);
        }

        // Get the current context ID
        uint64_t getCCID();

        // Get the prefetch context ID
        uint64_t getPCID();
    } rcr;
};

class LLBP_TAGE_64KB : public TAGE_SC_L_TAGE_64KB
{
    LLBP *parent;
    
    public:
    LLBP_TAGE_64KB(const LLBP_TAGE_64KBParams &p)
      : TAGE_SC_L_TAGE_64KB(p)
    {}

    void handleAllocAndUReset(bool alloc, bool taken,
                              TAGEBase::BranchInfo* bi, int nrand) override;

    void handleTAGEUpdate(Addr branch_pc, bool taken,
                          TAGEBase::BranchInfo* bi) override;
    int allocateEntry(int bank, TAGEBase::BranchInfo* bi, bool taken) override;
    bool isUseful(bool taken, TAGEBase::BranchInfo* bi) const override;
    bool isNotUseful(bool taken, TAGEBase::BranchInfo* bi) const override;

    void setParent(LLBP *p) {
        parent = p;
    }

    std::vector<int> alloc_banks;
};

} // namespace branch_prediction
} // namespace gem5

 #endif // __CPU_PRED_LLBP_HH__
