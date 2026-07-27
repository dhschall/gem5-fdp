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
#include "cpu/vp/eves/evtage_table.hh"

namespace gem5::eves
{

EVTAGE_table::EVTAGE_table(uint64_t logNumberOfEntries,
    unsigned bitsHistoryUsed, bool isBase)
 : logNumberOfEntries(logNumberOfEntries),
   bitsHistoryUsed(bitsHistoryUsed),
   isBase(isBase)
{
    table.resize(1ULL << logNumberOfEntries, {0, 0, 0, 0, false, 0});
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i].index = i;
    }
}

EVTAGE_Entry*
EVTAGE_table::lookup(ThreadID tid, Addr pc, uint64_t history)
{
    uint64_t index = getIndex(pc, history);
    uint64_t tag = getTag(pc, history);

    EVTAGE_Entry *entry = &table[index];

    if (!isBase) {
        if (entry->tag == tag && entry->tid == tid) {
            return entry;
        }
    } else {
        return entry; //No check, tagless
    }

    return nullptr;
}

EVTAGE_Entry*
EVTAGE_table::find(ThreadID tid, Addr pc, uint64_t history)
{
    uint64_t index = getIndex(pc, history);

    return &table[index];
}

EVTAGE_Entry*
EVTAGE_table::directAccess(std::size_t idx)
{
    assert(idx < table.size());
    return &table[idx];
}

void
EVTAGE_table::update(ThreadID tid, Addr pc,
    uint64_t history, EVTAGE_Entry *entry)
{
    assert(entry);
    entry->tag = getTag(pc, history);
    entry->tid = tid;

    //Value, left unchanged

    entry->confidence = 0;
    entry->useful = false;

    //Index, left unchanged
}

uint64_t
EVTAGE_table::xorFold(uint64_t pc, uint64_t usedHistory, unsigned size) const
{
    uint64_t mask = (1 << size) - 1;
    uint64_t fold = (usedHistory & mask);
    fold = (fold ^ (pc & mask));

    usedHistory = (usedHistory >> size);

    while (usedHistory) {
        fold = (fold ^ (usedHistory & mask));
        usedHistory = (usedHistory >> size);
    }

    return fold;
}

uint64_t
EVTAGE_table::getIndex(Addr pc, uint64_t history) const
{
    if (isBase) {
        uint64_t index = xorFold(0, pc, logNumberOfEntries);
        return index;

    } else {

        pc = (pc ^ (pc >> 2) ^ (pc >> 5));

        //So higher bits are older branches.
        uint64_t usedHistory = history >> (64 - bitsHistoryUsed);

        uint64_t index = xorFold(0, (pc ^ usedHistory), logNumberOfEntries);

        return index;
    }
}

uint64_t
EVTAGE_table::getTag(Addr pc, uint64_t history) const
{
    return pc; //Currently do only this
}

} //namespace gem5::eves
