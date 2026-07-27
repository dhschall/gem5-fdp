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
#include "cpu/vp/avpp/timing_stride_avpp_lvp.hh"

#include "cpu/o3/cpu.hh"
#include "debug/VP.hh"

namespace gem5::avpp
{
TimingStrideAvppLVP::TimingStrideAvppLVP(const TimingStrideAvppLVPParams &params)
    : TimingValuePredictor(params),
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
    DPRINTF(VP, "Creating Timing Stride AVPP Value Predictor\n");

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
TimingStrideAvppLVP::init()
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
TimingStrideAvppLVP::lookup(ThreadID tid, Addr inst_addr,
                    InstSeqNum seq_num)
{

    /*
    It is assumed that all loads request exactly 8 bytes.

    TODO: change for not making this assumption
    */

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

        ++lvpstats.AThitLookup;

        // ---------------------------
        // GENERATE PREDICTED ADDRESS
        // ---------------------------

        // The address is this instance + the stride
        addressResult.predictedAddress = addressEntry->predictedAddress + addressEntry->stride;
        // Notice that it doesn't take into accunt the in-flights as that is left to policies.

        // ---------------------------
        // GENERATE PREFETCH ADDRESS
        // ---------------------------

        // As this is a stride predictor, prefetching N positions ahead is simply adding N factor to stride
        addressResult.prefetchAddress = addressEntry->predictedAddress + (1 + addressEntry->pdis) * addressEntry->stride;

        // Only predict if the confidence is high enough
        addressResult.predict = addressEntry->confidence >= params.confidence_threshold;

        DPRINTF(VP,
            "Address Prediction for [pc=%#x, sn=%i]: lastaddr=%llu, stride=%i, "
            "conf=%i\n",
            inst_addr, seq_num, addressEntry->predictedAddress, addressEntry->stride,
            addressEntry->confidence);

        addressTable.accessEntry(addressEntry);

        //If the predict is valid and there is sufficient space in the block queue just in case:
        if (addressResult.predict && blockedPrefetchRequests.size() < params.size_prefetch_inflight_queue) {
            issuePrefetchLoad(inst_addr, tid, seq_num, addressResult.prefetchAddress);
        }
    } else {
        ++lvpstats.ATmissLookup;
    }

    // -------------------------------
    // SECOND LOOKUP (VT Table)
    // -------------------------------

    // The result of the value prediction:
    VPResult valueResult;
    valueResult.predict = false;

    if (addressResult.predict) {
        VTEntry::KeyType vtKey = indexVT(tid, addressResult.predictedAddress);
        VTEntry *valueEntry = valueTable.findEntry(vtKey);

        if (valueEntry && valueEntry->tid == tid) {
            //Predict the value directly
            valueResult.value = valueEntry->value;
            valueResult.predict = true;
            valueTable.accessEntry(valueEntry);
            ++lvpstats.VThitLookup;
        } else {
            ++lvpstats.VTmissLookup;
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
TimingStrideAvppLVP::updateWhenLoad(ThreadID tid, Addr inst_addr,
    InstSeqNum seq_num, Addr load_address, RegVal correct_val,
    RegVal predicted_val, bool value_generated, bool value_predicted,
    Cycles rn_to_ex_delay)
{
    //COMMIT UPDATE: update only the AT

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
        return;
    }

    //If there is an entry, update it
    addressTable.accessEntry(entry);

    Addr lastPredictedAddress = entry->predictedAddress + entry->stride;

    if (value_predicted && predictorUpdatePolicy == gem5::enums::PredictorUpdatePolicy::Correct) {
        panic_if(!(lastPredictedAddress == predicted_val), "Expected %llu, got %llu. Stride: %llu", predicted_val, lastPredictedAddress, entry->stride); //Should be the same, as not updates
    }

    int64_t stride = (int64_t)load_address - (int64_t)entry->predictedAddress;
    entry->predictedAddress = load_address;

    if (lastPredictedAddress != load_address) {
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
            ++entry->confidence;
        }

        if (entry->stride == 0) {
            ++lvpstats.constantAddressCorrect;
        } else {
            ++lvpstats.strideAddressCorrect;
        }
    }

    DPRINTF(VP,
        "Entry update: pred=%i, conf=%i, val=%li, stride=%li\n",
        predicted_val != correct_val, entry->confidence, entry->predictedAddress,
        entry->stride);
}

void
TimingStrideAvppLVP::updateWhenStore(ThreadID tid, Addr inst_addr,
    InstSeqNum seq_num, Addr store_address, const std::vector<uint8_t>& data_written,
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
TimingStrideAvppLVP::squashNotify(const InstSeqNum seq_num)
{

    DPRINTF(VP, "Squash inflight prediction until sn:%llu\n", seq_num);

    //Squash the prefetches:
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
TimingStrideAvppLVP::getPort(const std::string &if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "No support for vector ports");

    if (if_name == "prefetch_request_port") {
        return requestPort;
    } else {
        //Pass the burn to the parent class
        return TimingValuePredictor::getPort(if_name, idx);
    }
}

bool
TimingStrideAvppLVP::PrefetchRequestPort::recvTimingResp(PacketPtr pkt)
{
    return owner->ownerRecvTimingResp(pkt);
}

bool
TimingStrideAvppLVP::ownerRecvTimingResp(PacketPtr pkt)
{

    DPRINTF(VP, "Received packet %p cmd=%s response=%d\n",
        pkt, pkt->cmd.toString(), pkt->isResponse());

    //Change to dynamic_cast if problems.
    auto *state = dynamic_cast<gem5::avpp::fetchers::PrefetchSenderState*>(pkt->popSenderState());

    panic_if(!state, "Expected PrefetchSenderState in the return packet!");

    auto prefetchRequest = state->prefetchRequest;

    if (inflightPrefetchRequests.find(prefetchRequest) != inflightPrefetchRequests.end()) {
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
    } else {
        //In principle, this could happen if the prefetch got squashed, therefore, do nothing.

        delete state; //Free up memory of the state
        delete prefetchRequest; //Free up the memory of the packet and prefetch request in general
        return true;
    }
}

void
TimingStrideAvppLVP::PrefetchRequestPort::recvReqRetry()
{
    owner->ownerRecvReqRetry();
}

void
TimingStrideAvppLVP::ownerRecvReqRetry()
{
    while (!blockedPrefetchRequests.empty()) {
        auto prefetchRequest = blockedPrefetchRequests.front();

        assert(prefetchRequest->pkt->isRequest());
        DPRINTF(VP, "Sending prefetch packet RETRY %p cmd=%s\n",
        prefetchRequest->pkt, prefetchRequest->pkt->cmd.toString());
        if (!requestPort.sendTimingReq(prefetchRequest->pkt)) {
            DPRINTF(VP, "FAILED!\n");
            return; //Stop and wait, the port is blocked
        }

        DPRINTF(VP, "SUCCESFULL!\n");

        //Else: has been accepted
        blockedPrefetchRequests.pop_front();
    }
}

void
TimingStrideAvppLVP::ownerFinish(gem5::avpp::fetchers::PrefetchRequestPtr prefetchRequest, const Fault &fault)
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
        assert(prefetchRequest->pkt->isRequest());
        DPRINTF(VP, "Sending prefetch packet FIRST %p cmd=%s\n",
        prefetchRequest->pkt, prefetchRequest->pkt->cmd.toString());
        if (!requestPort.sendTimingReq(prefetchRequest->pkt)) { //Send the request
            //It fails! Add it to the retry queue
            blockedPrefetchRequests.push_back(prefetchRequest);
            DPRINTF(VP, "FAILED!\n");
        } else {
            DPRINTF(VP, "SUCCESFULL!\n");
        }
    } else {
        //Add it to the blocked queue directly, as the port is blocked
        blockedPrefetchRequests.push_back(prefetchRequest);
    }
}

void
TimingStrideAvppLVP::issuePrefetchLoad(Addr inst_addr, ThreadID tid, InstSeqNum seqNum, Addr prefetchAddress)
{
    assert(blockedPrefetchRequests.size() < params.size_prefetch_inflight_queue);

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
TimingStrideAvppLVP::indexAT(Addr inst_addr)
{
    return (inst_addr >> instShiftAmt);
}

TaggedEntry::KeyType
TimingStrideAvppLVP::indexAT(ThreadID tid, Addr inst_addr)
{
                                //address                          //secure
    return TaggedEntry::KeyType{(inst_addr >> instShiftAmt) ^ tid, false};
}

TaggedEntry::KeyType
TimingStrideAvppLVP::indexVT(ThreadID tid, Addr predicted_addr)
{
                            //address                 //secure
    return TaggedEntry::KeyType{predicted_addr ^ tid, false};

    //Here no instShiftAmt is used because it could be byte-aligned. I assume byte-addressable CPU.
}

//Register the statistics
TimingStrideAvppLVP::TimingStrideAvppLVPStats::TimingStrideAvppLVPStats(statistics::Group *parent) :
    statistics::Group(parent),
    ADD_STAT(ATmissLookup, statistics::units::Count::get(),
            "Number of AT misses at lookup"),
    ADD_STAT(AThitLookup, statistics::units::Count::get(),
            "Number of AT hits at lookup"),
    ADD_STAT(AThitLookupRate, statistics::units::Count::get(),
            "Rate of AT hits at lookup"),

    ADD_STAT(VTmissLookup, statistics::units::Count::get(),
            "Number of VT misses at lookup"),
    ADD_STAT(VThitLookup, statistics::units::Count::get(),
            "Number of VT hits at lookup"),
    ADD_STAT(VThitLookupRate, statistics::units::Count::get(),
            "Rate of VT hits at lookup"),

    ADD_STAT(constantAddressCorrect, statistics::units::Count::get(),
            "Number of correct checked predictions with constant address"),
    ADD_STAT(strideAddressCorrect, statistics::units::Count::get(),
            "Number of correct checked predictions with stride address"),

    ADD_STAT(valuePredSavedCyclesLog2, statistics::units::Count::get(),
            "Required for Top-Down, number of committed instructions"),
    ADD_STAT(valuePredSavedCycles, statistics::units::Count::get(),
            "Required for Top-Down, number of committed instructions")
{
    AThitLookupRate = AThitLookup / (AThitLookup + ATmissLookup);
    VThitLookupRate = VThitLookup / (VThitLookup + VTmissLookup);

    valuePredSavedCyclesLog2.init(0, 15, 1).flags(statistics::pdf);
    valuePredSavedCycles.init(0, 100, 10).flags(statistics::pdf);
}

} //namespace gem5::avpp
