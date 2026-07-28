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

#include "debug/VP.hh"

namespace gem5
{

TimingValuePredictor::TimingValuePredictor(
    const TimingValuePredictorParams &params)
    : ClockedObject(params),
    predictorUpdatePolicy(params.predictor_update_policy),
    predictorAvailabilityPolicy(params.predictor_availability_policy),
    inflightPendingUpdatePolicy(params.inflight_pending_update_policy),
    stats(this),
    lookupLatency(params.lookup_latency),
    updateLoadLatency(params.update_load_latency),
    updateStoreLatency(params.update_store_latency),
    maxLookupsPerCycle(params.max_lookups_per_cycle),
    maxUpdatesWhenLoadPerCycle(params.max_updates_when_load_per_cycle),
    maxUpdatesWhenStorePerCycle(params.max_updates_when_store_per_cycle),
    instShiftAmt(params.instShiftAmt),
    acceptedLookups(0),
    acceptedUpdateWhenLoad(0),
    acceptedUpdateWhenStore(0)
{

    //TODO: progressively keep implementing them to delete these panic_if

    panic_if(predictorUpdatePolicy
        == gem5::enums::PredictorUpdatePolicy::Speculative,
        "Currently speculative update policy is not supported!");

    panic_if(predictorAvailabilityPolicy
        == gem5::enums::PredictorAvailabilityPolicy::Delay,
        "Currently delay dispatch availability policy is not supported!");

    warn("Updating when store is currently unsupported.");
}

void
TimingValuePredictor::setO3CPU(gem5::o3::CPU *cpu) {
    this->cpu = cpu;
}

bool
TimingValuePredictor::canLookup()
{
    if (acceptedLookups < maxLookupsPerCycle) {
        return true;
    } else {
        return false;
    }
}

void
TimingValuePredictor::requestLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr, ThreadID,
                            Addr, InstSeqNum, VPResult)> callback)
{
    //ALL LOADS CALL THIS FUNCTION
    generalInflight.push_back({inst_addr, seq_num});
    ++stats.totalLoads;

    ++stats.lookupRequests;

    if (curCycle() != previousAcceptedLookups) {
        previousAcceptedLookups = curCycle();
        acceptedLookups = 0;
    }

    if (acceptedLookups < maxLookupsPerCycle) {
        //Process directly (The waiting queue is empty and more lookups can be accepted)
        processLookup(inst, tid, inst_addr, seq_num, callback);
        ++acceptedLookups;
        ++stats.lookupAccepted;
    }
}

void
TimingValuePredictor::requestUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_generated, bool value_predicted,
                                Cycles rn_to_ex_delay,
                                std::function<void()> callback)
{
    ++stats.updateWhenLoadRequests;

    //Regardless, update the real predict counter:
    if (value_predicted) {
        if (correct_val == predicted_val) {
            ++stats.realCorrectPredicted;
        } else {
            ++stats.realIncorrectPredicted;
        }
    }

    //Do this here, as generalInflight includes ALL loads, so it pops for all loads like this.
    assert(generalInflight.front().seqNum == seq_num);
    generalInflight.pop_front();

    if (curCycle() != previousAcceptedUpdateWhenLoad) {
        previousAcceptedUpdateWhenLoad = curCycle();
        acceptedUpdateWhenLoad = 0;
    }

    if (acceptedUpdateWhenLoad < maxUpdatesWhenLoadPerCycle) {
        //Process directly (The waiting queue is empty and more lookups can be accepted)
        processUpdateWhenLoad(tid, inst_addr, seq_num, load_address, correct_val,
            predicted_val, value_generated, value_predicted,
            rn_to_ex_delay, callback);
        ++acceptedUpdateWhenLoad;
        ++stats.updateWhenLoadAccepted;
    } else {
        if (value_predicted) {
            ++stats.ignoredPredicted;
        }
    }
}

void
TimingValuePredictor::requestUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    const std::vector<uint8_t>& data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order,
                                    std::function<void()> callback)
{
    ++stats.updateWhenStoreRequests;

    if (curCycle() != previousAcceptedUpdateWhenStore) {
        previousAcceptedUpdateWhenStore = curCycle();
        acceptedUpdateWhenStore = 0;
    }

    if (acceptedUpdateWhenStore < maxUpdatesWhenStorePerCycle) {
        //Process directly (The waiting queue is empty and more lookups can be accepted)
        processUpdateWhenStore(tid, inst_addr, seq_num, store_address,
            data_written, effective_size, guest_byte_order, callback);
        ++acceptedUpdateWhenStore;
        ++stats.updateWhenStoreAccepted;
    }
}

void
TimingValuePredictor::processLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr, ThreadID,
                            Addr, InstSeqNum, VPResult)> callback)
{
    //Create the lambda function and schedule the event
    auto lambda = [=, this] {
        finishLookup(inst, tid, inst_addr, seq_num, callback);
    };

    //Priority trick for ensuring FIFO
    auto event = new EventFunctionWrapper(lambda, name(), false, acceptedLookups);

    schedule(event, clockEdge(lookupLatency));

    //Create the inflight entry
    VPTimingInflightEvent timingInflight = {inst_addr, seq_num, event};

    lookupInflight.push_back(timingInflight);
}

void
TimingValuePredictor::processUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_generated, bool value_predicted,
                                Cycles rn_to_ex_delay,
                                std::function<void()> callback)
{
    //Create the lambda function and schedule the event
    auto lambda = [=, this] {
        finishUpdateWhenLoad(tid, inst_addr, seq_num, load_address, correct_val,
            predicted_val, value_generated, value_predicted,
            rn_to_ex_delay, callback);
    };

    //Priority trick for ensuring FIFO
    auto event = new EventFunctionWrapper(lambda, name(), false, acceptedUpdateWhenLoad);

    schedule(event, clockEdge(updateLoadLatency));

    //Create the inflight entry
    VPTimingInflightEvent timingInflight = {inst_addr, seq_num, event};

    updateWhenLoadInflight.push_back(timingInflight);
}

void
TimingValuePredictor::processUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    const std::vector<uint8_t>& data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order,
                                    std::function<void()> callback)
{
    //Create the lambda function and schedule the event
    auto lambda = [=, this] {
        finishUpdateWhenStore(tid, inst_addr, seq_num, store_address,
            data_written, effective_size, guest_byte_order, callback);
    };

    //Priority trick for ensuring FIFO
    auto event = new EventFunctionWrapper(lambda, name(), false, acceptedUpdateWhenStore);

    schedule(event, clockEdge(updateStoreLatency));

    //Create the inflight entry
    VPTimingInflightEvent timingInflight = {inst_addr, seq_num, event};

    updateWhenStoreInflight.push_back(timingInflight);
}

void
TimingValuePredictor::finishLookup(gem5::o3::DynInstPtr inst, ThreadID tid,
                        Addr inst_addr, InstSeqNum seq_num,
                        std::function<void(gem5::o3::DynInstPtr, ThreadID,
                            Addr, InstSeqNum, VPResult)> callback)
{
    assert(lookupInflight.front().seqNum == seq_num);

    VPResult predictionResult = lookup(tid, inst_addr, seq_num);

    if (predictionResult.predict) {
        ++stats.predicted;
    }

    callback(inst, tid, inst_addr, seq_num, predictionResult);

    //Destroy the event:
    delete lookupInflight.front().event;

    lookupInflight.pop_front();
}

void
TimingValuePredictor::finishUpdateWhenLoad(ThreadID tid, Addr inst_addr,
                                InstSeqNum seq_num, Addr load_address,
                                RegVal correct_val, RegVal predicted_val,
                                bool value_generated, bool value_predicted,
                                Cycles rn_to_ex_delay,
                                std::function<void()> callback)
{
    //Check that the events are completed in-order of how they where scheduled
    assert(updateWhenLoadInflight.front().seqNum == seq_num);

    updateWhenLoad(tid, inst_addr, seq_num, load_address,
            correct_val, predicted_val, value_generated,
            value_predicted, rn_to_ex_delay);

    if (value_predicted) {
        if (correct_val == predicted_val) {
            ++stats.correctPredicted;
        } else {
            ++stats.incorrectPredicted;
        }
    }

    callback(); //Notify commit stage that the update has finished.

    //Destroy the event:
    delete updateWhenLoadInflight.front().event;

    updateWhenLoadInflight.pop_front();
}


void
TimingValuePredictor::finishUpdateWhenStore(ThreadID tid, Addr inst_addr,
                                    InstSeqNum seq_num, Addr store_address,
                                    const std::vector<uint8_t>& data_written, unsigned effective_size,
                                    ByteOrder guest_byte_order,
                                    std::function<void()> callback)
{
    //Check that the events are completed in-order of how they where scheduled
    assert(updateWhenStoreInflight.front().seqNum == seq_num);

    updateWhenStore(tid, inst_addr, seq_num, store_address,
                data_written, effective_size, guest_byte_order);

    callback(); //Notify commit stage that the update has finished.

    //Destroy the event:
    delete updateWhenStoreInflight.front().event;

    updateWhenStoreInflight.pop_front();
}

void
TimingValuePredictor::squash(const InstSeqNum seq_num)
{
    //Perform parent things of squashing.
    //It is assumed that the inflight lists are seqNum ordered
    //(that should be true)

    while (!lookupInflight.empty() && lookupInflight.back().seqNum > seq_num) {
        auto event = lookupInflight.back().event;

        if (event) {
            if (event->scheduled()) {
                deschedule(event);
            }
            delete event;
            lookupInflight.back().event=nullptr;
            lookupInflight.pop_back();
        }
    }

    while (!updateWhenLoadInflight.empty() &&
            updateWhenLoadInflight.back().seqNum > seq_num) {
        auto event = updateWhenLoadInflight.back().event;

        if (event) {
            if (event->scheduled()) {
                deschedule(event);
            }
            delete event;
            updateWhenLoadInflight.back().event=nullptr;
            updateWhenLoadInflight.pop_back();
        }
    }

    while (!updateWhenStoreInflight.empty() &&
            updateWhenStoreInflight.back().seqNum > seq_num) {
        auto event = updateWhenStoreInflight.back().event;

        if (event) {
            if (event->scheduled()) {
                deschedule(event);
            }
            delete event;
            updateWhenStoreInflight.back().event=nullptr;
            updateWhenStoreInflight.pop_back();
        }
    }

    while (!generalInflight.empty() &&
            generalInflight.back().seqNum > seq_num) {
        generalInflight.pop_back();
    }

    squashNotify(seq_num); //Notify the child of the squash
}

uint64_t
TimingValuePredictor::countInflight(Addr inst_addr)
{
    uint64_t count = 0;
    for (auto &inflight: generalInflight) {
        if (inflight.instAddr == inst_addr) {
            ++count;
        }
    }

    return count;
}

TimingValuePredictor::TimingValuePredictorStats::TimingValuePredictorStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(totalLoads, statistics::units::Count::get(),
               "Total loads processed by the Load value predictor"),
      ADD_STAT(stallCycles, statistics::units::Count::get(),
               "Total loads processed by the Load value predictor"),
      ADD_STAT(stallEvents, statistics::units::Count::get(),
               "Total loads processed by the Load value predictor"),

    // ---------------------------------------------------------------------

      ADD_STAT(lookupRequests, statistics::units::Count::get(),
               "Number of VP lookups requested"),
      ADD_STAT(lookupAccepted, statistics::units::Count::get(),
               "Number of VP lookups accepted"),
      ADD_STAT(lookupAcceptedRate, statistics::units::Count::get(),
               "Rate of VP lookups accepted"),

      ADD_STAT(predicted, statistics::units::Count::get(),
               "Number of loads classified as predictable"),
      ADD_STAT(predCoverage, statistics::units::Count::get(),
               "Rate of covered loads predicted compared to accepted lookups"),
      ADD_STAT(aparentAccuracy, statistics::units::Count::get(),
               "Accuracy of the predictions, based on the ones checked"),
      ADD_STAT(realAccuracy, statistics::units::Count::get(),
               "Accuracy of the predictions, accounting all predictions"),

    // ---------------------------------------------------------------------

      ADD_STAT(updateWhenLoadRequests, statistics::units::Count::get(),
               "Number of VP update when load requested"),
      ADD_STAT(updateWhenLoadAccepted, statistics::units::Count::get(),
               "Number of VP update when load accepted"),
      ADD_STAT(updateWhenLoadAcceptedRate, statistics::units::Count::get(),
               "Rate of VP update when load accepted"),

      ADD_STAT(correctPredicted, statistics::units::Count::get(),
               "Number of loads correctly checked that predicted correctly"),
      ADD_STAT(realCorrectPredicted, statistics::units::Count::get(),
               "Number of total predicted loads that predicted correctly"),

      ADD_STAT(incorrectPredicted, statistics::units::Count::get(),
               "Number of loads correctly checked that predicted incorrectly"),
      ADD_STAT(realIncorrectPredicted, statistics::units::Count::get(),
               "Number of total predicted loads that predicted incorrectly"),

      ADD_STAT(ignoredPredicted, statistics::units::Count::get(),
               "Number of loads predicted that haven't been checked"),
      ADD_STAT(ignoredPredictedRate, statistics::units::Count::get(),
               "Rate of loads predicted that haven't been predicted against total predicted"),

    // ---------------------------------------------------------------------

      ADD_STAT(updateWhenStoreRequests, statistics::units::Count::get(),
               "Number of VP update when store requested"),
      ADD_STAT(updateWhenStoreAccepted, statistics::units::Count::get(),
               "Number of VP update when store accepted"),
      ADD_STAT(updateWhenStoreAcceptedRate, statistics::units::Count::get(),
               "Rate of VP update when store accepted")

{
    lookupAcceptedRate = lookupAccepted / lookupRequests;
    predCoverage = predicted / lookupAccepted;
    aparentAccuracy = correctPredicted / (correctPredicted + incorrectPredicted);
    //(realCorrectPredicted + realIncorrectPredicted) SHALL BE EQUAL TO predicted
    realAccuracy = realCorrectPredicted / (realCorrectPredicted + realIncorrectPredicted);

    updateWhenLoadAcceptedRate = updateWhenLoadAccepted / updateWhenLoadRequests;
    //(realCorrectPredicted + realIncorrectPredicted) SHALL BE EQUAL TO predicted
    ignoredPredictedRate = ignoredPredicted / (realCorrectPredicted + realIncorrectPredicted);

    updateWhenStoreAcceptedRate = updateWhenStoreAccepted / updateWhenStoreRequests;
}

} //namespace gem5
