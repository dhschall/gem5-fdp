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

#include "cpu/vp/timing_value_predictor.hh"

namespace gem5
{

TimingValuePredictor::TimingValuePredictor(
    const TimingValuePredictorParams &params)
    : ClockedObject(params),
    predictorUpdatePolicy(params.predictor_update_policy),
    predictorAvailabilityPolicy(params.predictor_availability_policy),
    inflightPendingUpdatePolicy(params.inflight_pending_update_policy),
    lookupLatency(params.lookup_latency),
    updateLoadLatency(params.update_load_latency),
    updateStoreLatency(params.update_store_latency),
    instShiftAmt(params.instShiftAmt),
    stats(this),
{

    //TODO: progressively keep implementing them to delete these panic_if

    panic_if(predictorUpdatePolicy
        == gem5::enums::PredictorUpdatePolicy::Speculative,
        "Currently speculative update policy is not supported!");

    panic_if(predictorAvailabilityPolicy
        == gem5::enums::PredictorAvailabilityPolicy::Delay,
        "Currently delay dispatch availability policy is not supported!");

    panic_if(inflightPendingUpdatePolicy
        == gem5::enums::InflightPendingUpdatePolicy::InflightWait,
        "Currently in-flight wait pending update policy is not supported!");
}

void
TimingValuePredictor::setO3CPU(gem5::o3::CPU *cpu) {
    this->cpu = cpu;
}

void
TimingValuePredictor::startLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr inst,
                            ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num, VPResult result)>
                            callback)
{
    //Create the lambda function and schedule the event
    auto lambda = [=, this] {
        finishLookup(inst, tid, inst_addr, seq_num, callback);
    };

    auto event = EventFunctionWrapper(lambda, name());

    schedule(event, clockEdge(lookupLatency));

    //Create the inflight entry
    VPTimingInflight timingInflight = {inst_addr, seq_num, event};

    lookupInflight.push_back(timingInflight);
}

void
TimingValuePredictor::finishLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr inst,
                            ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num, VPResult result)>
                            callback)
{
    //Check that the events are completed in-order of how they where scheduled
    assert(lookupInflight.front().seqNum == seq_num);

    VPResult predictionResult = lookup(tid, inst_addr, seq_num);

    callback(inst, tid, inst_addr, seq_num, predictionResult);

    lookupInflight.pop_front();
}

void
TimingValuePredictor::startUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr load_address,
                                    RegVal correct_val, RegVal predicted_val,
                                    bool value_predicted, Cycles rn_to_ex_delay)
{
    auto lambda = [=, this] {
        finishUpdateWhenLoad(tid, inst_addr, seq_num, load_address, correct_val,
            predicted_val, value_predicted, rn_to_ex_delay);
    };

    auto event = EventFunctionWrapper(lambda, name());

    schedule(event, clockEdge(updateLoadLatency));

    //Create the inflight entry
    VPTimingInflight timingInflight = {inst_addr, seq_num, event};

    updateWhenLoadInflight.push_back(timingInflight);
}

void
TimingValuePredictor::finishUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr load_address,
                                    RegVal correct_val, RegVal predicted_val,
                                    bool value_predicted, Cycles rn_to_ex_delay)
{
    //Check that the events are completed in-order of how they where scheduled
    assert(updateWhenLoadInflight.front().seqNum == seq_num);

    updateWhenLoad(tid, inst_addr, seq_num, load_address,
            correct_val, predicted_val, value_predicted, rn_to_ex_delay);

    updateWhenLoadInflight.pop_front();
}

void
TimingValuePredictor::startUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    uint8_t* data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order)
{
    auto lambda = [=, this] {
        finishUpdateWhenStore(tid, inst_addr, seq_num, store_address,
            data_written, effective_size, guest_byte_order);
    };

    auto event = EventFunctionWrapper(lambda, name());

    schedule(event, clockEdge(updateStoreLatency));
}

void
TimingValuePredictor::finishUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    uint8_t* data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order)
{
    //Check that the events are completed in-order of how they where scheduled
    assert(updateWhenStoreInflight.front().seqNum == seq_num);

    updateWhenStore(tid, inst_addr, seq_num, store_address,
                data_written, effective_size, guest_byte_order);

    updateWhenStoreInflight.pop_front();
}

void
TimingValuePredictor::squash(const InstSeqNum seq_num)
{
    //Perform parent things of squashing.
    //It is assumed that the inflight lists are seqNum ordered
    //(that should be true)

    while (!lookupInflight.empty() && lookupInflight.back().seqNum > seq_num) {
        lookupInflight.pop_back();
    }

    while (!updateWhenLoadInflight.empty() &&
            updateWhenLoadInflight.back().seqNum > seq_num) {
        updateWhenLoadInflight.pop_back();
    }

    while (!updateWhenStoreInflight.empty() &&
            updateWhenStoreInflight.back().seqNum > seq_num) {
        updateWhenStoreInflight.pop_back();
    }

    squashNotify(seq_num); //Notify the child of the squash
}

TimingValuePredictor::TimingValuePredictorStats::TimingValuePredictorStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of VP lookups"),
      ADD_STAT(misses, statistics::units::Count::get(),
               "Number of VP misses (No update available)"),
      ADD_STAT(predictableLoads, statistics::units::Count::get(),
               "Number of loads classified as predictable"),
      ADD_STAT(predicted, statistics::units::Count::get(),
               "Number of loads classified as predictable"),
      ADD_STAT(correct, statistics::units::Count::get(),
               "Number of loads correctly classified as predictable"),
      ADD_STAT(incorrect, statistics::units::Count::get(),
               "Number of loads incorrectly classified as predictable"),
      ADD_STAT(predCoverage, statistics::units::Count::get(),
               "Number covered loads predicted compared to all loads"),
      ADD_STAT(accuracy, statistics::units::Count::get(),
               "Accuracy of the predictions"),
      ADD_STAT(constLoads, statistics::units::Count::get(),
               "Number of loads classified as constant"),
      ADD_STAT(constLoadsCorrect, statistics::units::Count::get(),
               "Number of constant loads correctly predicted"),
      ADD_STAT(constLoadsIncorrect, statistics::units::Count::get(),
               "Number of constant loads incorrectly predicted"),
      ADD_STAT(totalLoads, statistics::units::Count::get(),
               "Total loads processed by the Load value predictor"),
      ADD_STAT(numZeroConstLoads, statistics::units::Count::get(),
               "Number of constant loads with value 0"),
      ADD_STAT(numOneConstLoads, statistics::units::Count::get(),
               "Number of constant loads with value 1")
{
    predicted = correct + incorrect;
    predCoverage = predicted / totalLoads;
    accuracy = correct / predicted;
}

} //namespace gem5
