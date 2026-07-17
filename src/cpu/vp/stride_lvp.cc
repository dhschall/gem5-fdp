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
 *
 * @brief Simple stride load value predictor
 */

#include "cpu/vp/stride_lvp.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/VP.hh"

namespace gem5
{

StrideLVP::StrideLVP(const StrideLVPParams &p)
    : AtomicValuePredictor(p),
      vpTable("VPT", p.table_entries, p.table_assoc,
              p.table_replacement_policy, p.table_indexing_policy,
              LVPEntry(genTagExtractor(p.table_indexing_policy))),
      confThreshold(p.confidence_threshold),
      confResetToZero(p.confidence_reset_to_zero),
      useStride(p.use_stride),
      lvpstats(this)
{
    DPRINTF(VP, "Creating Stride Value Predictor\n");

    if (!isPowerOf2(p.table_entries)) {
        fatal("VP prediction table entries is not a power of 2!");
    }
}

VPResult
StrideLVP::lookup(ThreadID tid, Addr inst_addr, InstSeqNum seq_num)
{
    stats.lookups++;

    // Addr idx = index(inst_addr);
    LVPEntry::KeyType key = index(tid, inst_addr);
    LVPEntry *entry = vpTable.findEntry(key);
    VPResult result;
    result.predict = false;

    if (entry && entry->tid == tid) {
        // Get the number of inflights
        unsigned inflights = numInflights(inst_addr);
        // The value is this instance + in-flights * the stride
        result.value = entry->value + ((inflights + 1) * entry->stride);

        // Only predict if the confidence is high enough
        result.predict = entry->confidence >= confThreshold;
        DPRINTF(VP,
                "VP for [pc=%#x, sn=%i]: lastval=%llu, stride=%i, "
                "inflights=%i, conf=%i\n",
                inst_addr, seq_num, entry->value, entry->stride, inflights,
                entry->confidence);

        vpTable.accessEntry(entry);
    }

    // Push the prediction instance to the in-flight queue
    inflightPred.push_front({inst_addr, seq_num});

    DPRINTF(VP,
            "VP::%s(iaddr=%#x, sn=%i) res:[pred=%i, value=%llu] IFsize=%i\n",
            __func__, inst_addr, seq_num, result.predict, result.value,
            inflightPred.size());

    return result;
}

void
StrideLVP::updateWhenLoad(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
                  Addr load_address, RegVal correct_val, RegVal predicted_val,
                  bool value_predicted, Cycles rn_to_ex_delay)
{

    stats.totalLoads++;
    if (value_predicted) {
        if (predicted_val != correct_val) {
            stats.incorrect++;
        } else {
            stats.correct++;
        }
    }
    DPRINTF(VP,
            "VP::%s(iaddr=%#x, sn=%llu, pval=%#x, cval=%#x, pred=%i) "
            "correct=%i, delay=%i\n",
            __func__, inst_addr, seq_num, predicted_val, correct_val,
            value_predicted, predicted_val == correct_val, rn_to_ex_delay);

    LVPEntry::KeyType key = index(tid, inst_addr);
    LVPEntry *entry = vpTable.findEntry(key);

    if (entry == nullptr) {
        entry = vpTable.findVictim(key);
        vpTable.insertEntry(key, entry);

        entry->confidence = 0;
        entry->tid = tid;
        entry->stride = 0;
        entry->value = correct_val;
        DPRINTF(VP, "Allocate new entry: %llu\n", entry->value);
        assert(inflightPred.size());
        assert(inflightPred.back().sn == seq_num);
        inflightPred.pop_back();
        return;
    }

    vpTable.accessEntry(entry);
    uint64_t last_value = entry->value;
    uint64_t pred_value = entry->value + entry->stride;
    int64_t stride =
        useStride ? ((int64_t)correct_val - (int64_t)last_value) : 0;

    // Update the latest value + stride
    entry->value = correct_val;
    // DPRINTF(VP, "Size: %i\n", inflightPred.size());
    assert(inflightPred.size());
    assert(inflightPred.back().sn == seq_num);
    inflightPred.pop_back();

    if (pred_value != correct_val) {
        if (entry->confidence > 0) {
            entry->confidence = confResetToZero ? 0 : entry->confidence - 1;
        }

        if (entry->confidence == 0) {
            entry->stride = stride;
        }
    } else {
        uint64_t d = rn_to_ex_delay;
        lvpstats.valuePredSavedCyclesLog2.sample(d > 0 ? floorLog2(d) : 0);
        lvpstats.valuePredSavedCycles.sample(rn_to_ex_delay);

        if (entry->confidence < confThreshold) {
            entry->confidence++;
        }
        if (entry->stride == 0) {
            lvpstats.constantCorrect++;
            if (entry->value == 0) {
                lvpstats.numZeroConstLoads++;
            } else if (entry->value == 1) {
                lvpstats.numOneConstLoads++;
            }
        } else {
            lvpstats.strideCorrect++;
        }
    }

    DPRINTF(VP,
            "Entry update: pred=%i, conf=%i, val=%li, stride=%li, IFsize=%i\n",
            pred_value != correct_val, entry->confidence, entry->value,
            entry->stride, inflightPred.size());
}

void
StrideLVP::squash(InstSeqNum seq_num)
{
    DPRINTF(VP, "Squash inflight prediction until sn:%llu\n", seq_num);
    while (!inflightPred.empty() && inflightPred.front().sn > seq_num) {
        inflightPred.pop_front();
    }
}

unsigned
StrideLVP::numInflights(Addr iaddr)
{
    unsigned n = 0;
    for (auto &e : inflightPred) {
        if (e.iaddr == iaddr) {
            n++;
        }
    }
    return n;
}

Addr
StrideLVP::index(Addr addr)
{
    return (addr >> instShiftAmt);
}

StrideLVP::LVPEntry::KeyType
StrideLVP::index(ThreadID tid, Addr inst_addr)
{
    return LVPEntry::KeyType{(inst_addr >> instShiftAmt) ^ tid, false};
}

StrideLVP::StrideLVPStats::StrideLVPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(constantCorrect, statistics::units::Count::get(),
               "Number of VP lookups"),
      ADD_STAT(strideCorrect, statistics::units::Count::get(),
               "Number of VP lookups"),
      ADD_STAT(numZeroConstLoads, statistics::units::Count::get(),
               "Number of constant loads with value 0"),
      ADD_STAT(numOneConstLoads, statistics::units::Count::get(),
               "Number of constant loads with value 1"),
      ADD_STAT(valuePredSavedCyclesLog2, statistics::units::Count::get(),
               "Required for Top-Down, number of committed instructions"),
      ADD_STAT(valuePredSavedCycles, statistics::units::Count::get(),
               "Required for Top-Down, number of committed instructions")
{
    valuePredSavedCyclesLog2.init(0, 15, 1).flags(statistics::pdf);
    valuePredSavedCycles.init(0, 100, 10).flags(statistics::pdf);
}

} // namespace gem5
