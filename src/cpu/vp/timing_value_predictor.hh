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

#ifndef __CPU_TIMING_VALUE_PREDICTOR_HH__
#define __CPU_TIMING_VALUE_PREDICTOR_HH__

#include <deque>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "enums/ByteOrder.hh"
#include "params/TimingValuePredictor.hh"
#include "sim/clocked_object.hh"

//Forward declaration
namespace gem5::o3 {
  class CPU;
}

namespace gem5
{

struct VPTimingInflight
{

    Addr instAddr;
    InstSeqNum seqNum;

    EventFunctionWrapper event;
};

/** Represents the result of the value predictor.  */
struct VPResult
{
    RegVal value;
    bool predict;
};

class TimingValuePredictor : public ClockedObject
{
    public:
        TimingValuePredictor(const TimingValuePredictorParams &params);

        void setO3CPU(gem5::o3::CPU *cpu);

        /*Children should override the ones that actually perform real work,
          The semantic ones: lookup, updateWhenLoad, updateWhenStore.
          This updates one are optional as you may not train.
        */

        // GETS CALLED DURING FETCH
        void startLookup(ThreadID tid, Addr inst_addr,
                        InstSeqNum seq_num,
                        std::function<void(VPResult result)> callback);

        void finishLookup(ThreadID tid, Addr inst_addr,
                        InstSeqNum seq_num,
                        std::function<void(VPResult result)> callback);

        virtual VPResult lookup(ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num) = 0;

        // GETS CALLED DURING COMMIT
        void startUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_predicted, Cycles rn_to_ex_delay);

        void finishUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_predicted, Cycles rn_to_ex_delay);

        virtual void updateWhenLoad(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr load_address,
                                    RegVal correct_val, RegVal predicted_val,
                                    bool value_predicted, Cycles rn_to_ex_delay)
                                    {};

        // GETS CALLED DURING COMMIT
        void startUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    uint8_t* data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order);

        void finishUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    uint8_t* data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order);

        virtual void updateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    uint8_t* data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order)
                                    {};


        void squash(const InstSeqNum seq_num); //The "real" squash function

        // If predict error, squash the inflight instructions in value predictor.
        // GETS (hopefully not) CALLED DURING COMMIT

        /** Notifies the child of the squash */
        virtual void squashNotify(const InstSeqNum seq_num) {};

    protected:
        Cycles lookupLatency;

        Cycles updateLoadLatency;

        Cycles updateStoreLatency;

        /** Instruction shift amount */
        const unsigned instShiftAmt;

        /** Pointer to the O3 CPU used */
        gem5::o3::CPU *cpu;

        struct TimingValuePredictorStats : public statistics::Group
        {
            TimingValuePredictorStats(statistics::Group *parent);

            statistics::Scalar lookups;
            statistics::Scalar misses;
            statistics::Scalar predictableLoads;
            statistics::Formula predicted;
            statistics::Scalar correct;
            statistics::Scalar incorrect;
            statistics::Formula predCoverage;
            statistics::Formula accuracy;
            statistics::Scalar constLoads;
            statistics::Scalar constLoadsCorrect;
            statistics::Scalar constLoadsIncorrect;
            statistics::Scalar totalLoads;

            statistics::Scalar numZeroConstLoads;
            statistics::Scalar numOneConstLoads;

        } stats;

    private:

        //Deques containing the inflight events + metadata

        std::deque<VPTimingInflight> lookupInflight;
        std::deque<VPTimingInflight> updateWhenLoadInflight;
        std::deque<VPTimingInflight> updateWhenStoreInflight;

};

} //namespace gem5

#endif //__CPU_TIMING_VALUE_PREDICTOR_HH__
