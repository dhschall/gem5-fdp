/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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
 */

#ifndef __CPU_O3_PHAST_HH__
#define __CPU_O3_PHAST_HH__

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "dyn_inst_ptr.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "mem/port.hh"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <deque>
#include <bitset>

using namespace std;

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

struct PredictionResult;

#define BITSETSIZE 500

class MemDepUnit;

class PHAST
{

  public:

    class SimplBlockCache;

    /** Default constructor.  init() must be called prior to use. */
    PHAST() { };

    /** Creates PHAST predictor with given table sizes. */
    PHAST(const BaseO3CPUParams &params, MemDepUnit *mem_dep_unit);

    /** Default destructor. */
    ~PHAST();

    /** Initializes the PHAST predictor with the given table sizes. */
    void init(const BaseO3CPUParams &params, MemDepUnit *mem_dep_unit);

    /** Records a memory ordering violation between the younger load
    * and the older store. */
    void violation(Addr load_pc, InstSeqNum load_seq_num, InstSeqNum store_seq_num, Addr store_pc, std::ptrdiff_t storeQueueDistance, bool predicted, unsigned predictedPathInex, uint64_t predictedHash, BranchHistory branchHistory);

    /** Checks if the instruction with the given PC is dependent upon
    * any store.  @return Returns the relative SQ distance of the store
    * instruction this PC is dependent upon.  Returns -1 if none.
    */
    PredictionResult checkInst(Addr load_pc, InstSeqNum load_seq_num, BranchHistory branchHistory, bool isLoad);

    /** Updates predictor at load commit */
    void commit(Addr load_pc, Addr load_addr, unsigned load_size, Addr store_addr, unsigned store_size, unsigned path_index, uint64_t predictor_hash);

    /** Clears all tables */
    void clear();

    /** mem_dep_unit interface methods that don't do anything in PHAST */
    void squash(InstSeqNum squashed_num, ThreadID tid) { return; };
    void issued(Addr issued_PC, InstSeqNum issued_seq_num, bool is_store) { return; }
    void insertStore(Addr store_PC, InstSeqNum store_seq_num, ThreadID tid) { return; };
    void insertLoad(Addr load_PC, InstSeqNum load_seq_num) { return;}

    unsigned selectedTargetBits;

    uint64_t selectedTargetMask;

  private:

    bool debug;

    unsigned depCheckShift;

    //largest seen index into branchSizes
    unsigned maxBranches;

    std::vector<unsigned> historySizes;

    std::vector<SimplBlockCache> paths;

    unsigned maxHistory;

    unsigned entriesPerTable;

    MemDepUnit *memDepUnit;

    uint64_t generateBranchHash(unsigned path_index, unsigned num_branches, BranchHistory branch_history, unsigned start_indx);

    uint64_t foldHistory(std::bitset<BITSETSIZE> h, int bits, unsigned _set_bits, unsigned _tag_bits);

    public:

    class SimplBlockCache {
        struct Entry {
            uint64_t tag;
            std::ptrdiff_t distance;
            uint32_t lru;
            uint32_t counter;
        };

        uint32_t setBits;
        uint32_t tagBits;
        uint32_t associativity;
        uint64_t lruCounter;
        unsigned maxCounterValue;
        std::vector<std::vector<Entry>> cache;

        public:
        uint64_t xorFold(uint64_t pc, uint64_t history, unsigned size) const;

        uint64_t getIndex(Addr pc, uint64_t history) const;

        uint64_t getTag(Addr pc, uint64_t history) const;

        Entry* findEntry(Addr pc, uint64_t history);

        Entry* getLRUEntry(uint64_t set);

        void updateLRU(Entry* entry);

            int init(uint32_t set_bits, uint32_t _associativity, uint32_t tag_bits, uint32_t max_counter_value);

            std::ptrdiff_t predict(Addr pc, uint64_t history);

            void update(Addr pc, uint64_t history, std::ptrdiff_t);

            void updateCommit(Addr pc, uint64_t history, bool predictionWrong);

            void clear();

            unsigned getSetBits() { return setBits; }

            unsigned getTagBits() { return tagBits; }

            void printBlock(uint64_t set);
    };

};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_PHAST_HH__
