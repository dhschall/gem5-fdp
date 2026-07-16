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

#ifndef __CPU_AVPP_STRIDE_AVPP_LVP_HH__
#define __CPU_AVPP_STRIDE_AVPP_LVP_HH__

#include <deque>
#include <unordered_set>

#include "arch/generic/mmu.hh"
#include "base/cache/associative_cache.hh"
#include "base/types.hh"
#include "cpu/o3/lsq_unit.hh"
#include "cpu/random/lsfr16.hh"
#include "cpu/vp/avpp/entries.hh"
#include "cpu/vp/value_predictor.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "mem/port.hh"
#include "params/StrideAvppLVP.hh"

//Forward definition of the PrefetchTranslationRequest
namespace gem5::avpp::fetchers
{
    struct PrefetchRequest;
    using PrefetchRequestPtr = PrefetchRequest*;
}

namespace gem5::avpp
{

class StrideAvppLVP : public ValuePredictor
{
    public:

        friend struct gem5::avpp::fetchers::PrefetchRequest;

        StrideAvppLVP(const StrideAvppLVPParams &params);

        VPResult lookup(ThreadID tid, Addr inst_addr, InstSeqNum seq_num) override;
        void updateWhenLoad(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
            Addr load_address, RegVal correct_val, RegVal predicted_val,
            bool value_predicted, Cycles rn_to_ex_delay) override;
        void updateWhenStore(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
            Addr store_address, uint8_t *data_written,
            unsigned effective_size, ByteOrder guest_byte_order) override;
        void squash(const InstSeqNum seq_num) override;

        void init() override;

        Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;

    private:

        const StrideAvppLVPParams &params;

        RequestorID requestorID;

        class PrefetchRequestPort : public RequestPort
        {
            private:
                StrideAvppLVP *owner;
            public:
                PrefetchRequestPort(const std::string& name, StrideAvppLVP *owner) :
                    RequestPort(name), owner(owner)
                { }
            protected:
                /** Gets called when the data arribes from the cache */
                bool recvTimingResp(PacketPtr pkt) override;
                /** Gets called when the cache was busy */
                void recvReqRetry() override;
        };

        //Owner versions of the port functions
        bool ownerRecvTimingResp(PacketPtr pkt);
        void ownerRecvReqRetry();

        PrefetchRequestPort requestPort;

        //Owner versions of the translation process
        void ownerFinish(gem5::avpp::fetchers::PrefetchRequestPtr prefetchRequest, const Fault &fault);

        /** Stores in-flight prefetch requests by the memory system. */
        std::unordered_set<gem5::avpp::fetchers::PrefetchRequestPtr> inflightPrefetchRequests;

        /** Stores the blocked prefetch requests in order for retrying them. */
        std::deque<gem5::avpp::fetchers::PrefetchRequestPtr> blockedPrefetchRequests;


        /** AT table */
        AssociativeCache<ATEntry> addressTable; //IN THIS CONTEXT: this is a stride predictor -> same PC = stride load address.

        /** VT table */
        AssociativeCache<VTEntry> valueTable;

        /** Random number generator, for calculating prefetch distance update */
        LFSR16 lfsr16;

        /** Issues a prefetch load for a certain address. */
        void issuePrefetchLoad(Addr inst_addr, ThreadID tid, InstSeqNum seqNum, Addr prefetchAddress);

        /** Returns the index for AT table, based on the instruction address */
        Addr indexAT(Addr inst_addr);
        /** Returns the index for AT table, based on the instruction address. Takes into account SMT */
        TaggedEntry::KeyType indexAT(ThreadID tid, Addr inst_addr);

        /** Returns the index for VT table, based on the predicted address. Takes into account SMT */
        TaggedEntry::KeyType indexVT(ThreadID tid, Addr predicted_addr);

        /** Inflight prediction information */
        struct InflightInfo
        {
            Addr iaddr;
            InstSeqNum sn;
        };
        /** Track inflight predicted instructions */
        std::deque<InflightInfo> inflightPred;

        /** Calculates how many inflight instances of the same instruction are they */
        unsigned numInflights(Addr iaddr);

        /** The statistics that this component adds */
        struct StrideAvppLVPStats : public statistics::Group
        {
            StrideAvppLVPStats(statistics::Group *parent);

            statistics::Scalar constantCorrect;
            statistics::Scalar strideCorrect;
            statistics::Scalar numZeroConstLoads;
            statistics::Scalar numOneConstLoads;

            statistics::Distribution valuePredSavedCyclesLog2;
            statistics::Distribution valuePredSavedCycles;
        } lvpstats;
};

} //namespace gem5::avpp

#endif //__CPU_AVPP_STRIDE_AVPP_LVP_HH__
