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
#include "cpu/vp/eves/eves.hh"

namespace gem5::eves
{

EVES::EVES(const EVESParams &params)
 : TimingValuePredictor(params),
   estride(),
   evtage(params.evtage_log_table_sizes,
    params.evtage_table_history_bits),
   lvpstats(this)
{}

void
EVES::setO3CPU(gem5::o3::CPU *cpu)
{
    TimingValuePredictor::setO3CPU(cpu);
    evtage.setO3CPU(cpu);
}

VPResult
EVES::lookup(ThreadID tid, Addr inst_addr,
            InstSeqNum seq_num, VPTimingInflight &entry)
{
    //Request the two predictors and pick the one with higher confidence.
    auto estrideResponse = estride.lookup(tid, inst_addr,
            seq_num, countInflight(inst_addr));

    auto evtageResponse = evtage.lookup(tid, inst_addr, seq_num, entry);

    if (estrideResponse.result.predict
        && !evtageResponse.result.predict)
    {
        //EStride provides, EVTAGE doesn't
        return estrideResponse.result;

    } else if (!estrideResponse.result.predict
        && evtageResponse.result.predict)
    {
        //EVTAGE provides, EStride doesn't
        return evtageResponse.result;

    } else if (estrideResponse.result.predict) {

        //Both provide -> check confidence
        if (estrideResponse.confidence
            > evtageResponse.confidence)
        {
            return estrideResponse.result;
        } else {
            return evtageResponse.result;
            //This also means that in case of same
            //priority EVTAGE gets preference.
            //THIS IS DONE ON PURPOSE
        }

    } else {
        //None provides
        return {0, false};
    }
}

void
EVES::updateWhenLoad(ThreadID tid, Addr inst_addr,
            InstSeqNum seq_num, Addr load_address,
            RegVal correct_val, RegVal predicted_val,
            bool value_generated, bool value_predicted,
            InflightState *state, Cycles rn_to_ex_delay)
{
    //Update both predictors
    auto success = evtage.updateWhenLoad(tid, inst_addr, seq_num,
        load_address, correct_val, predicted_val,
        value_generated, value_predicted, state, rn_to_ex_delay);

    if (!success)
        ++lvpstats.EVTAGEupdatesBlocked;

    estride.updateWhenLoad(tid, inst_addr, seq_num, load_address,
        correct_val, predicted_val, value_generated,
        value_predicted, rn_to_ex_delay);
}

//Register the statistics
EVES::EVESStats::EVESStats(statistics::Group *parent) :
    statistics::Group(parent),
    ADD_STAT(EVTAGEupdatesBlocked, statistics::units::Count::get(),
            "Number of updates that were blocked because didn't"
            " produce a value")
{}

} //namespace gem5::eves
