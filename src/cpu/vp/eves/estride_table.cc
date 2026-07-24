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
#include "cpu/vp/eves/estride_table.hh"

namespace gem5::eves
{

EStrideTable::EStrideTable()
{
    //Initialize all entries as invalid and empty
    for (auto &x: table) {
        for (auto &y: x) {
            y = {0, 0, 0, 0, 0, 0, false};
        }
    }
}

EStrideEntry*
EStrideTable::lookup(ThreadID tid, Addr pc)
{
    uint8_t idx[3] = {
        hash0(tid, pc),
        hash1(tid, pc),
        hash2(tid, pc)
    };

    EStrideEntry* entry = nullptr;

    unsigned count = 0;
    if (table[0][idx[0]].tag == pc && table[0][idx[0]].tid == tid) {
        ++count;
        entry = &table[0][idx[0]];
    }

    if (table[1][idx[1]].tag == pc && table[1][idx[1]].tid == tid) {
        ++count;
        entry = &table[1][idx[1]];
    }

    if (table[2][idx[2]].tag == pc && table[2][idx[2]].tid == tid) {
        ++count;
        entry = &table[2][idx[2]];
    }

    assert(count < 2);
    return entry;
}

void
EStrideTable::allocate(ThreadID tid, Addr pc, const EStrideEntry &entry)
{
    assert(entry.confidence == 0);
    assert(entry.valid);


    uint8_t idx[3] = {
        hash0(tid, pc),
        hash1(tid, pc),
        hash2(tid, pc)
    };

    //Pick the victim
    EStrideEntry* picked = nullptr;
    uint8_t useful_seen = -1;

    for (int i = 0; i < 3; ++i) {
        if (!table[i][idx[i]].valid) {
            picked = &table[i][idx[i]];
            break;
        }
        if (table[i][idx[i]].useful < useful_seen) {
            useful_seen = table[i][idx[i]].useful;
            picked = &table[i][idx[i]];
        }
    }

    assert(picked);
    *picked = entry;
}

uint8_t
EStrideTable::hash0(ThreadID tid, Addr addr)
{
    return (addr ^ tid) & 0xF;
}

uint8_t
EStrideTable::hash1(ThreadID tid, Addr addr)
{
    return ((addr ^ (addr >> 4)) ^ (tid ^ (tid >> 3))) & 0xF;
}

uint8_t
EStrideTable::hash2(ThreadID tid, Addr addr)
{
    return ((addr ^ (addr >> 8) ^ (addr >> 13))
        ^ (tid ^ (tid >> 7) ^ (tid >> 13))) & 0xF;
}

} //namespace gem5::eves
