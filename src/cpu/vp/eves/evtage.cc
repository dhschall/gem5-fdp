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
#include "cpu/vp/eves/evtage.hh"

#include <random>

#include "cpu/o3/cpu.hh"

namespace gem5::eves
{

EVTAGE::EVTAGE(const EVTAGEParams &params)
 : TimingValuePredictor(params),
   lvpstats(this)
{
    unsigned numOfTables = params.log_table_sizes.size();

    //Higher indeces -> larger history
    tables.emplace_back(params.log_table_sizes[0], 0, true);
    for (int i = 0; i < numOfTables - 1; i++) {

        tables.emplace_back(params.log_table_sizes[i+1],
            params.table_history_bits[i], false);
    }
}

VPResult
EVTAGE::lookup(ThreadID tid, Addr inst_addr,
            InstSeqNum seq_num)
{
    uint64_t branchHistory = getHistory(seq_num);

    //Higher indeces -> larger history
    std::vector<EVTAGE_Entry*> candidateEntries;
    std::vector<std::size_t> tableIdx;

    for (std::size_t i = 0; i < tables.size(); ++i) {
        auto entry = tables[i].lookup(tid, inst_addr, branchHistory);

        if (entry) {
            candidateEntries.push_back(entry);
            tableIdx.push_back(i);
        }
    }

    //At least the candidate from the first table.
    assert(candidateEntries.size());
    assert(tableIdx.size());

    //provider is the one that has the longest history
    EVTAGE_Entry *provider = candidateEntries.back();

    VPResult result;
    result.predict = false;

    //3-bit counter is saturated
    if (provider->confidence >= 7) {
        result.value = provider->value;
        result.predict = true;
    }

    inflightPredictions.push_back({tableIdx.back(),
        provider->index, result.predict, seq_num});

    return result;
}

void
EVTAGE::updateWhenLoad(ThreadID tid, Addr inst_addr,
            InstSeqNum seq_num, Addr load_address,
            RegVal correct_val, RegVal predicted_val,
            bool value_generated, bool value_predicted,
            Cycles rn_to_ex_delay)
{
    if (!value_generated) {
        ++lvpstats.updatesBlocked;
        return;
    }

    assert(inflightPredictions.front().seqNum == seq_num);
    auto &info = inflightPredictions.front();

    auto entry = tables[info.table_idx].directAccess(info.index);
    assert(entry);

    //Replace only the value when the confidence is zero
    if (entry->confidence == 0) {
        entry->value = correct_val;
    }

    if (predicted_val == correct_val) {
        if (entry->confidence < 7) {
            ++entry->confidence;
        }
        entry->useful = true;
    } else {
        entry->confidence = 0;
        entry->useful = false;
    }

    // -----------------------------------------------
    // Replacement

    std::vector<EVTAGE_Entry*> upperEntriesAll;
    std::vector<EVTAGE_Entry*> upperEntriesNotUseful;
    std::vector<EVTAGE_table*> upperEtnriesNotUsefulTables;

    uint64_t branchHistory = getHistory(seq_num);

    for (std::size_t i = info.table_idx + 1; i < tables.size(); ++i) {

        auto entry = tables[i].find(tid, inst_addr, branchHistory);

        upperEntriesAll.push_back(entry);
        if (!entry->useful) {
            upperEntriesNotUseful.push_back(entry);
            upperEtnriesNotUsefulTables.push_back(&tables[i]);
        }
    }

    if (upperEntriesNotUseful.size()) {
        //There are not-useful entries
        auto idx = randomIndex(upperEtnriesNotUsefulTables.size());

        upperEntriesNotUseful[idx]->value = correct_val;

        upperEtnriesNotUsefulTables[idx]->update(tid, inst_addr,
            branchHistory, upperEntriesNotUseful[idx]);

    } else {
        for (auto &e : upperEntriesAll) {
            e->useful = false;
        }
    }
}

void
EVTAGE::squashNotify(const InstSeqNum seq_num)
{
    while (!inflightPredictions.empty()
        && inflightPredictions.back().seqNum > seq_num)
    {
        inflightPredictions.pop_back();
    }
}

uint64_t
EVTAGE::getHistory(InstSeqNum seq_num)
{
    assert(cpu);

    using BranchHistory = gem5::o3::BranchHistory;

    //Get a copy to the decodedBranchHistory
    BranchHistory decodedBranchHistory = cpu->getDecode()->getBranchHistory();

    //Get the branch history of the branches that are really before the load
    while (!decodedBranchHistory.empty()
        && decodedBranchHistory.front().seqNum > seq_num)
    {
        decodedBranchHistory.pop_front();
    }

    //Convert to a 64-bit history.
    uint64_t branchHistory = 0;

    const std::size_t bits = decodedBranchHistory.size() > 64 ?
        64 : decodedBranchHistory.size();

    for (std::size_t i = 0; i < bits; ++i) {
        if (decodedBranchHistory[i].taken) {
            branchHistory |= (1ULL << i);
        }
    }

    return branchHistory;
}

std::size_t
EVTAGE::randomIndex(std::size_t size)
{
    assert(size != 0);

    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<std::size_t> dist(0, size-1);

    return dist(gen);
}

//Register the statistics
EVTAGE::EVTAGEStats::EVTAGEStats(statistics::Group *parent) :
    statistics::Group(parent),
    ADD_STAT(updatesBlocked, statistics::units::Count::get(),
            "Number of updates that were blocked because didn't"
            " produce a value")
{}

} //namespace gem5::eves
