/*
 * Copyright (c) 2026 Technical Unversity of Munich
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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
#ifndef __CPU_EVES_EVTAGE_TABLE_HH__
#define __CPU_EVES_EVTAGE_TABLE_HH__

#include <vector>

#include "base/types.hh"

namespace gem5::eves
{

struct EVTAGE_Entry
{
    Addr tag; //The whole PC, temporal solution
    ThreadID tid; //also used as tag

    uint64_t value;

    uint8_t confidence; //3-bit
    bool useful;

    //Bookeeping information:
    std::size_t index;
};

class EVTAGE_table
{
    public:

        EVTAGE_table(uint64_t logNumberOfEntries,
            unsigned bitsHistoryUsed, bool isBase);

        /** Performs a lookup */
        EVTAGE_Entry* lookup(ThreadID tid, Addr pc, uint64_t history);

        /** Finds an entry */
        EVTAGE_Entry* find(ThreadID tid, Addr pc, uint64_t history);

        /** Directly accesses an entry */
        EVTAGE_Entry* directAccess(std::size_t idx);

        /** Updates a certain entry information. */
        void update(ThreadID tid, Addr pc,
            uint64_t history, EVTAGE_Entry *entry);

    private:

        uint64_t xorFold(uint64_t pc,
            uint64_t usedHistory, unsigned size) const;

        uint64_t getIndex(Addr pc, uint64_t history) const;

        uint64_t getTag(Addr pc, uint64_t history) const;

        uint64_t logNumberOfEntries;
        uint64_t bitsHistoryUsed;

        /** Whether this table is a tagless one without use of history */
        bool isBase;

        std::vector<EVTAGE_Entry> table;
};

} //namespace gem5::eves

#endif //__CPU_EVES_EVTAGE_TABLE_HH__
