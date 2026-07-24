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
#include "cpu/vp/avpp/atomic_stride_avpp_lvp.hh"

#include <bit>

#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/lsq_unit.hh"
#include "cpu/vp/avpp/inject_fetcher.hh"
#include "debug/VP.hh"
#include "mem/packet_access.hh"
#include "sim/byteswap.hh"

namespace gem5::avpp
{

AtomicStrideAvppLVP::AtomicStrideAvppLVP(const AtomicStrideAvppLVPParams &params)
    : AtomicValuePredictor(params),
    params(params),
    requestPort(params.name + ".prefetch_request_port", this),
    addressTable("VT", params.at_table_entries, params.at_table_assoc,
                params.at_table_replacement_policy, params.at_table_indexing_policy,
                ATEntry(genTagExtractor(params.at_table_indexing_policy))),
    valueTable("VT", params.vt_table_entries, params.vt_table_assoc,
                params.vt_table_replacement_policy, params.vt_table_indexing_policy,
                VTEntry(genTagExtractor(params.vt_table_indexing_policy))),
    lfsr16(),
    lvpstats(this)
    {
        DPRINTF(VP, "Creating Atomic Stride AVPP Value Predictor\n");

        //Do some checks:
        if (!isPowerOf2(params.at_table_entries)) {
            fatal("Address table entries is not a power of 2!");
        }

        if (!isPowerOf2(params.vt_table_entries)) {
            fatal("Value table entries is not a power of 2!");
        }

        if (params.prob_up > 15) {
            fatal("Currently probUp can be as minimum as 1/32768 (p=15) and as maximum of 1/1 (p=0)");
        }
    }

void
AtomicStrideAvppLVP::init()
{
    //Check that the port is initalized
    if (!requestPort.isConnected()) {
        fatal("%s: prefetch_request_port is not connected. "
            "Please connect it in the Python configuration.\n",
            name());
    }
    requestorID = cpu->system->getRequestorId(this);
}

VPResult
AtomicStrideAvppLVP::lookup(ThreadID tid, Addr inst_addr, InstSeqNum seq_num)
{

    /*
    It is assumed that all loads request exactly 8 bytes.

    TODO: change for not making this assumption
    */

    stats.lookups++;

    // -------------------------------
    // FIRST LOOKUP (AT Table)
    // -------------------------------

    //Look-up in the AT table
    ATEntry::KeyType atKey = indexAT(tid, inst_addr); //Notice that VTEntry::KeyType is the same as TaggedEntry::KeyType (because inheritance)
    ATEntry *addressEntry = addressTable.findEntry(atKey);

    //The result of the address prediction:
    ATResult addressResult;
    addressResult.predict = false;

    if (addressEntry && addressEntry->tid == tid) {
        // Get the number of inflights
        unsigned inflights = numInflights(inst_addr);

        // ---------------------------
        // GENERATE PREDICTED ADDRESS
        // ---------------------------

        // The address is this instance + in-flights * the stride
        addressResult.predictedAddress = addressEntry->predictedAddress + ((inflights + 1) * addressEntry->stride);

        // ---------------------------
        // GENERATE PREFETCH ADDRESS
        // ---------------------------

        //As this is a stride predictor, prefetching N positions ahead is simply adding N factor to stride
        addressResult.prefetchAddress = addressEntry->predictedAddress + ((inflights + 1 + addressEntry->pdis) * addressEntry->stride);

        // Only predict if the confidence is high enough
        addressResult.predict = addressEntry->confidence >= params.confidence_threshold;
        DPRINTF(VP,
            "Address Prediction for [pc=%#x, sn=%i]: lastaddr=%llu, stride=%i, "
            "inflights=%i, conf=%i\n",
            inst_addr, seq_num, addressEntry->predictedAddress, addressEntry->stride, inflights,
            addressEntry->confidence);

        addressTable.accessEntry(addressEntry);

        //If the predict is valid and there is sufficient space in the block queue just in case:
        if (addressResult.predict && blockedPrefetchRequests.size() < params.size_prefetch_inflight_queue) {
            issuePrefetchLoad(inst_addr, tid, seq_num, addressResult.prefetchAddress);
        }
    }

    //Push the address prediction in the front. Notice that one address prediction is always equal to one value prediction.
    inflightPred.push_front({inst_addr, seq_num});

    // -------------------------------
    // SECOND LOOKUP (VT Table)
    // -------------------------------

    //The result of the value prediction:
    VPResult valueResult;
    valueResult.predict = false;

    if (addressResult.predict) {
        VTEntry::KeyType vtKey = indexVT(tid, addressResult.predictedAddress); //Notice that VTEntry::KeyType is the same as TaggedEntry::KeyType (because inheritance)
        VTEntry *valueEntry = valueTable.findEntry(vtKey);

        if (valueEntry && valueEntry->tid == tid) {
            //Predict value directly
            valueResult.value = valueEntry->value;
            valueResult.predict = true;
            valueTable.accessEntry(valueEntry);
        } else {
            //NO ENTRY -> performing prefetch distance updating

            //Check if a prefetch for the predicted address is present
            bool isPresent = false;
            for (auto prefetchRequest: inflightPrefetchRequests) {
                if (prefetchRequest->vaddr == addressResult.predictedAddress) {
                    isPresent = true;
                }
            }

            if (isPresent) {
                if (addressEntry->d) { //==1, ==increase
                    //Should increase with probability probUp
                    uint16_t top_bits = (params.prob_up == 0) ? 0 : (lfsr16.next() >> (16 - params.prob_up));
                    if (top_bits == 0 && addressEntry->pdis != params.pdis_max_value) {
                        ++addressEntry->pdis;
                    }
                } else { //==0, ==decrease
                    addressEntry->d = true;
                }
            } else {
                if (!addressEntry->d) { //==0, ==decrease
                    //Should decrease with probability probUp
                    uint16_t top_bits = (params.prob_up == 0) ? 0 : (lfsr16.next() >> (16 - params.prob_up));
                    if (top_bits == 0 && addressEntry->pdis != 0) {
                        --addressEntry->pdis;
                    }
                } else { //==1, ==increase
                    addressEntry->d = false;
                }
            }
        }
    }

    return valueResult;
}

void
AtomicStrideAvppLVP::updateWhenLoad(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
    Addr load_address, RegVal correct_val, RegVal predicted_val,
    bool value_predicted, Cycles rn_to_ex_delay)
{

    //COMMIT UPDATE: update only the AT

    stats.totalLoads++;

    //Updating the AT table with the load address
    ATEntry::KeyType key = indexAT(tid, inst_addr);
    ATEntry *entry = addressTable.findEntry(key);

    //If there was no entry allocate one
    if (!entry) {
        entry = addressTable.findVictim(key);
        addressTable.insertEntry(key, entry);

        entry->confidence = 0;
        entry->tid = tid;
        entry->stride = 0;
        entry->predictedAddress = load_address;
        DPRINTF(VP, "Allocate new entry; %llu\n", entry->predictedAddress);
        assert(inflightPred.size()); //Check that there is are at least one inflight instruction
        assert(inflightPred.back().sn == seq_num); //check that the last inflight instruction is this one.
        inflightPred.pop_back(); //pop it
        return;
    }

    //If there is an entry, update it
    addressTable.accessEntry(entry);
    Addr lastPredictedAddress = entry->predictedAddress;
    Addr lastPredictionValue = entry->predictedAddress + entry->stride; //The value that was predicted
    int64_t stride = (int64_t)load_address - (int64_t)lastPredictedAddress;
    entry->predictedAddress = load_address;

    assert(inflightPred.size());
    assert(inflightPred.back().sn == seq_num);
    inflightPred.pop_back();

    if (lastPredictionValue != correct_val) {
        if (entry->confidence > 0) {
            entry->confidence = params.confidence_reset_to_zero ? 0 : (entry->confidence - 1);
        }

        if (entry->confidence == 0) {
            entry->stride = stride;
        }
    } else {
        uint64_t d = rn_to_ex_delay;
        lvpstats.valuePredSavedCyclesLog2.sample(d > 0 ? floorLog2(d) : 0);
        lvpstats.valuePredSavedCycles.sample(rn_to_ex_delay);

        if (entry->confidence < params.confidence_threshold) {
                entry->confidence++;
        }
        if (entry->stride == 0) {
            lvpstats.constantCorrect++;
            if (predicted_val == 0) {
                lvpstats.numZeroConstLoads++;
            } else if (predicted_val == 1) {
                lvpstats.numOneConstLoads++;
            }
        } else {
            lvpstats.strideCorrect++;
        }
    }

    DPRINTF(VP,
        "Entry update: pred=%i, conf=%i, val=%li, stride=%li, IFsize=%i\n",
        predicted_val != correct_val, entry->confidence, entry->predictedAddress,
        entry->stride, inflightPred.size());
}

void
AtomicStrideAvppLVP::updateWhenStore(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
    Addr store_address, const std::vector<uint8_t>& data_written,
    unsigned effective_size, ByteOrder guest_byte_order)
{

    //First of all, check that there is a load in the same store address. I.e. a VT entry exists
    /*
    IMPORTANT NOTE: By doing these we assume that the load and the store go to the exact same address.
    Yet, the store could be misaligned respect to the load and cover partially its range.
    TODO: add support for this edge case (result: should improve accuracy of the predictor a little bit)
    */
    VTEntry::KeyType key = indexVT(tid, store_address);
    VTEntry *entry = valueTable.findEntry(key);

    if (!entry) {
        return; //Do nothing
    }

    //There is an entry:
    valueTable.accessEntry(entry);

    //get the whole data that is going to be stored:
    uint64_t value = 0;
    assert(effective_size <= sizeof(value));

    memcpy(&value, data_written.data(), effective_size);

    value = gtoh(value, guest_byte_order);

    entry->value = value; //That is all!
}

void
AtomicStrideAvppLVP::squash(const InstSeqNum seq_num)
{
    DPRINTF(VP, "Squash inflight prediction until sn:%llu\n", seq_num);
    while (!inflightPred.empty() && inflightPred.front().sn > seq_num) {
        inflightPred.pop_front();
    }

    //Squash also the prefetches:
    for (auto it = inflightPrefetchRequests.begin(); it != inflightPrefetchRequests.end(); ) {
        auto prefetchRequest = *it;

        if (prefetchRequest->seqNum > seq_num) {
            it = inflightPrefetchRequests.erase(it);
        } else {
            ++it;
        }
    }

    //Check the blocked queue
    for (auto it = blockedPrefetchRequests.begin(); it != blockedPrefetchRequests.end(); ) {
        auto prefetchRequest = *it;

        if (prefetchRequest->seqNum > seq_num) {
            it = blockedPrefetchRequests.erase(it);
        } else {
            ++it;
        }
    }
}

Port&
AtomicStrideAvppLVP::getPort(const std::string &if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "No support for vector ports");

    if (if_name == "speculative_request_port") {
        return requestPort;
    } else {
        //Pass the burn to the parent class
        return AtomicValuePredictor::getPort(if_name, idx);
    }
}

bool
AtomicStrideAvppLVP::PrefetchRequestPort::recvTimingResp(PacketPtr pkt)
{
    return owner->ownerRecvTimingResp(pkt);
}

bool
AtomicStrideAvppLVP::ownerRecvTimingResp(PacketPtr pkt)
{

    //Change to dynamic_cast if problems.
    auto *state = static_cast<gem5::avpp::fetchers::PrefetchSenderState*>(pkt->popSenderState());

    panic_if(!state, "Expected PrefetchSenderState in the return packet!");

    auto prefetchRequest = state->prefetchRequest;

    ThreadID tid = prefetchRequest->tid;
    //Currently not used!
    //InstSeqNum seqNum = prefetchRequest->seqNum;
    delete state; //Free up memory of the state

    //Get the data to update the VT table
    uint64_t value = pkt->getLE<uint64_t>(); //NOTE: ASSUMES LITTLE ENDIAN
    Addr prefetchAddress = prefetchRequest->vaddr;

    //Access the VT table:
    VTEntry::KeyType key = indexVT(tid, prefetchAddress);
    VTEntry *entry = valueTable.findEntry(key);

    //Update the entry
    if (!entry) {
        entry = valueTable.findVictim(key);
        valueTable.insertEntry(key, entry);

        entry->tid = tid;
        entry->value = value;
    } else {
        valueTable.accessEntry(entry);

        entry->tid = tid;
        entry->value = value;
    }

    auto erased = inflightPrefetchRequests.erase(prefetchRequest);
    assert(erased == 1);

    delete prefetchRequest; //Free up the memory of the packet and prefetch request in general
    return true;
}

void
AtomicStrideAvppLVP::PrefetchRequestPort::recvReqRetry()
{
    owner->ownerRecvReqRetry();
}

void
AtomicStrideAvppLVP::ownerRecvReqRetry()
{

    while (!blockedPrefetchRequests.empty()) {
        auto prefetchRequest = blockedPrefetchRequests.front();

        if (!requestPort.sendTimingReq(prefetchRequest->pkt)) {
            return; //Stop and wait, the port is blocked
        }

        //Else: has been accepted
        blockedPrefetchRequests.pop_back();
    }
}

void
AtomicStrideAvppLVP::ownerFinish(gem5::avpp::fetchers::PrefetchRequestPtr prefetchRequest, const Fault &fault)
{
    if (fault != NoFault) {
        //Abort the whole prefetch

        //Check that the prefetch request exists
        assert(inflightPrefetchRequests.find(prefetchRequest) != inflightPrefetchRequests.end());

        auto erased = inflightPrefetchRequests.erase(prefetchRequest);
        assert(erased == 1);

        delete prefetchRequest;
        return;
    }

    //Send the actual request.
    assert(prefetchRequest->req->hasPaddr());
    prefetchRequest->createPkt();

    //Add the sender state for then recovering:
    prefetchRequest->pkt->pushSenderState(new gem5::avpp::fetchers::PrefetchSenderState(prefetchRequest));

    if (blockedPrefetchRequests.empty()) {
        //The port is ready to recieve some more requests, sent it:
        if (!requestPort.sendTimingReq(prefetchRequest->pkt)) { //Send the request
            //It fails! Add it to the retry queue
            blockedPrefetchRequests.push_back(prefetchRequest);
        }
    } else {
        //Add it to the blocked queue directly, as the port is blocked
        blockedPrefetchRequests.push_back(prefetchRequest);
    }
}

void
AtomicStrideAvppLVP::issuePrefetchLoad(Addr inst_addr, ThreadID tid, InstSeqNum seqNum, Addr prefetchAddress)
{

    assert(inflightPrefetchRequests.size() < params.size_prefetch_inflight_queue);

    //Aliases:
    using PrefetchRequestPtr = gem5::avpp::fetchers::PrefetchRequestPtr;
    using PrefetchRequest = gem5::avpp::fetchers::PrefetchRequest;

    auto lambda = [this](PrefetchRequestPtr prefetchRequest, const Fault &fault) {
        ownerFinish(prefetchRequest, fault);
    };

    PrefetchRequestPtr prefetchRequest = new PrefetchRequest(cpu, requestorID, prefetchAddress, tid, seqNum, lambda);

    inflightPrefetchRequests.insert(prefetchRequest);

    prefetchRequest->startTranslation();
}

Addr
AtomicStrideAvppLVP::indexAT(Addr inst_addr)
{
    return (inst_addr >> instShiftAmt);
}

TaggedEntry::KeyType
AtomicStrideAvppLVP::indexAT(ThreadID tid, Addr inst_addr)
{
                                //address                          //secure
    return TaggedEntry::KeyType{(inst_addr >> instShiftAmt) ^ tid, false};
}

TaggedEntry::KeyType
AtomicStrideAvppLVP::indexVT(ThreadID tid, Addr predicted_addr)
{
                            //address                 //secure
    return TaggedEntry::KeyType{(predicted_addr >> 3) ^ tid, false};

    //Aligned to 8 bytes, as each VT entry stores that.
}

unsigned
AtomicStrideAvppLVP::numInflights(Addr iaddr)
{
    unsigned n = 0;
    for (auto &e : inflightPred) {
        if (e.iaddr == iaddr) {
            n++;
        }
    }
    return n;
}

//Register the statistics
AtomicStrideAvppLVP::AtomicStrideAvppLVPStats::AtomicStrideAvppLVPStats(statistics::Group *parent) :
    statistics::Group(parent),
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

} //namespace gem5::avpp
