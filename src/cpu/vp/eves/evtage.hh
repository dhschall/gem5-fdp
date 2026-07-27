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
#ifndef __CPU_EVES_EVTAGE_LVP_HH__
#define __CPU_EVES_EVTAGE_LVP_HH__

#include <vector>

#include "cpu/vp/eves/evtage_table.hh"
#include "cpu/vp/timing_value_predictor.hh"
#include "params/EVTAGE.hh"

namespace gem5::eves
{

class EVTAGE : public TimingValuePredictor
{
    public:
        EVTAGE(const EVTAGEParams &params);

    protected:

        VPResult lookup(ThreadID tid, Addr inst_addr,
                    InstSeqNum seq_num) override;

        void updateWhenLoad(ThreadID tid, Addr inst_addr,
                    InstSeqNum seq_num, Addr load_address,
                    RegVal correct_val, RegVal predicted_val,
                    bool value_predicted, Cycles rn_to_ex_delay)
                    override;

        void squashNotify(const InstSeqNum seq_num) override;

    private:
        std::vector<EVTAGE_table> tables;

        struct InflightPrediction
        {
            std::size_t table_idx;
            std::size_t index;
            bool predicted;
            InstSeqNum seqNum;
        };

        std::deque<InflightPrediction> inflightPredictions;

        uint64_t getHistory();

        std::size_t randomIndex(std::size_t size);
};

} //namespace gem5::eves


#endif
