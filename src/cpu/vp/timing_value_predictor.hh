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
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/vp/structs.hh"
#include "enums/ByteOrder.hh"
#include "enums/InflightPendingUpdatePolicy.hh"
#include "enums/PredictorAvailabilityPolicy.hh"
#include "enums/PredictorUpdatePolicy.hh"
#include "params/TimingValuePredictor.hh"
#include "sim/clocked_object.hh"

//Forward declaration
namespace gem5::o3 {
  class CPU;
}

namespace gem5
{

struct VPTimingInflightEvent
{

    Addr instAddr;
    InstSeqNum seqNum;

    EventFunctionWrapper *event;
};

struct VPTimingInflight
{
    Addr instAddr;
    InstSeqNum seqNum;
};

class TimingValuePredictor : public ClockedObject
{
    public:
        TimingValuePredictor(const TimingValuePredictorParams &params);

        void setO3CPU(gem5::o3::CPU *cpu);

        /*Children should override the ones that actually perform real work,
          The semantic ones: lookup, updateWhenLoad, updateWhenStore, squashNotify.
          The updates one are optional as you may not train.
        */

        // GETS CALLED DURING FETCH
        void requestLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr inst,
                            ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num, VPResult result)>
                            callback);

        bool canLookup();

        // GETS CALLED DURING COMMIT
        void requestUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_predicted, Cycles rn_to_ex_delay,
                                std::function<void()> callback);

        // GETS CALLED DURING COMMIT
        void requestUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    const std::vector<uint8_t>& data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order,
                                    std::function<void()> callback);

        void squash(const InstSeqNum seq_num); //The "real" squash function

        void registerLoad(Addr inst_addr, InstSeqNum seq_num);

        // If predict error, squash the inflight instructions in value predictor.
        // GETS (hopefully not) CALLED DURING COMMIT

        //the internal policies

        gem5::enums::PredictorUpdatePolicy predictorUpdatePolicy;
        gem5::enums::PredictorAvailabilityPolicy predictorAvailabilityPolicy;
        gem5::enums::InflightPendingUpdatePolicy inflightPendingUpdatePolicy;

        /** Checks if there is at least one in-flight instace
         * of a certain instruction.
         * Returns true if yes, false if not.
         */
        bool checkInflightWait(Addr inst_addr);

    private:

        void processLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr inst,
                            ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num, VPResult result)>
                            callback);

        void finishLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr inst,
                            ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num, VPResult result)>
                            callback);

        void processUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_predicted, Cycles rn_to_ex_delay,
                                std::function<void()> callback);

        void finishUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_predicted, Cycles rn_to_ex_delay,
                                std::function<void()> callback);

        void processUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    const std::vector<uint8_t>& data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order,
                                    std::function<void()> callback);

        void finishUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    const std::vector<uint8_t>& data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order,
                                    std::function<void()> callback);

    protected:

        virtual VPResult lookup(ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num) = 0;

        virtual void updateWhenLoad(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr load_address,
                                    RegVal correct_val, RegVal predicted_val,
                                    bool value_predicted, Cycles rn_to_ex_delay)
                                    {};

        virtual void updateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    const std::vector<uint8_t>& data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order)
                                    {};

        /** Notifies the child of the squash */
        virtual void squashNotify(const InstSeqNum seq_num) {};

        //Variables defining latencies
        Cycles lookupLatency;
        Cycles updateLoadLatency;
        Cycles updateStoreLatency;

        //Variables defining maxRequests/cycle (i.e. ports)
        const unsigned maxLookupsPerCycle;
        const unsigned maxUpdatesWhenLoadPerCycle;
        const unsigned maxUpdatesWhenStorePerCycle;

        /** Instruction shift amount */
        const unsigned instShiftAmt;

        /** Pointer to the O3 CPU used */
        gem5::o3::CPU *cpu;

    private:

        //Deques containing the inflight events + metadata
        std::deque<VPTimingInflightEvent> lookupInflight;
        std::deque<VPTimingInflightEvent> updateWhenLoadInflight;
        std::deque<VPTimingInflightEvent> updateWhenStoreInflight;

        //Mark how many requests accepted in a particular cycle
        unsigned acceptedLookups;
        unsigned acceptedUpdateWhenLoad;
        unsigned acceptedUpdateWhenStore;

        Cycles previousAcceptedLookups;
        Cycles previousAcceptedUpdateWhenLoad;
        Cycles previousAcceptedUpdateWhenStore;

        /** General in-flight loads */
        std::deque<VPTimingInflight> generalInflight;

        struct TimingValuePredictorStats : public statistics::Group
        {
            TimingValuePredictorStats(statistics::Group *parent);

            statistics::Scalar totalLoads;

            // ---------------------------------------------

            statistics::Scalar lookupRequests;
            statistics::Scalar lookupAccepted;
            statistics::Formula lookupAcceptedRate;

            statistics::Scalar predicted;
            statistics::Formula predCoverage;
            statistics::Formula aparentAccuracy;
            statistics::Formula realAccuracy;

            // ---------------------------------------------

            statistics::Scalar updateWhenLoadRequests;
            statistics::Scalar updateWhenLoadAccepted;
            statistics::Formula updateWhenLoadAcceptedRate;

            statistics::Scalar correctPredicted;
            statistics::Scalar realCorrectPredicted;

            statistics::Scalar incorrectPredicted;
            statistics::Scalar realIncorrectPredicted;

            statistics::Scalar ignoredPredicted;
            statistics::Formula ignoredPredictedRate;

            // ---------------------------------------------

            statistics::Scalar updateWhenStoreRequests;
            statistics::Scalar updateWhenStoreAccepted;
            statistics::Formula updateWhenStoreAcceptedRate;

        } stats;
};

} //namespace gem5

#endif //__CPU_TIMING_VALUE_PREDICTOR_HH__
