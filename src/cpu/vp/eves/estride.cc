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
#include "cpu/vp/eves/estride.hh"

#include "debug/VP.hh"

namespace gem5::eves
{

EStride::EStride(const EStrideParams &params)
 : TimingValuePredictor(params)
{}

VPResult
EStride::lookup(ThreadID tid, Addr inst_addr,
            InstSeqNum seq_num)
{

    VPResult result;
    result.predict = false;

    EStrideEntry *entry = table.lookup(tid, inst_addr);

    unsigned confidence = 0;
    if (entry) {
        confidence = entry->confidence;
    }
    if (entry && entry->confidence >= 7) {

        // Unfortunately, this actually doesn't take into account
        // different threads.

        result.value = entry->value
            + (countInflight(inst_addr) + 1) * entry->stride;
        result.predict = true;
    }

    DPRINTF(VP, "Performing lookup of seqNum: %llu."
        "PC: %llx. Confidence=%llu. Predicted=%i. Does entry exist? %i\n",
        seq_num, inst_addr, confidence, result.predict, (bool)entry);

    return result;
}

void
EStride::updateWhenLoad(ThreadID tid, Addr inst_addr,
            InstSeqNum seq_num, Addr load_address,
            RegVal correct_val, RegVal predicted_val,
            bool value_predicted, Cycles rn_to_ex_delay)
{

    DPRINTF(VP,
        "Performing updateWhenLoad of seqNum: %llu. PC: %llx\n",
        seq_num, inst_addr);

    EStrideEntry *entry = table.lookup(tid, inst_addr);

    if (entry) {
        //There is entry, update it

        //Calculate what value would predict NOW, based on the last commited:
        uint64_t predNow = entry->value + entry->stride;
        uint64_t lastValue = entry->value;
        entry->value = correct_val;

        // The stride is incorrect
        if (predNow != correct_val) {
            entry->stride = correct_val - lastValue;

            //Reduce confidence by 4 and reset useful
            if (entry->confidence < 4) {
                entry->confidence = 0;
            } else {
                entry->confidence -= 4;
            }
            entry->useful = 0;

        } else { //Everything is correct now
            //Increment confidence and useful
            if (entry->confidence < 32) {
                ++entry->confidence;
            }
            if (entry->useful < 4) {
                ++entry->useful;
            }
        }

    } else {
        //NO ENTRY, consider allocating one
        //We will assume intially that 100% allocate
        EStrideEntry a = {inst_addr, tid, correct_val, 0, 0, 0, true};
        table.allocate(tid, inst_addr, a);
        DPRINTF(VP, "ALLOCATING ENTRY FOR THIS PC\n");
    }
}

} //namespace gem5::eves
