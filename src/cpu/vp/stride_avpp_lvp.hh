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

#ifndef __CPU_VP_STRIDE_AVPP_LVP_HH__
#define __CPU_VP_STRIDE_AVPP_LVP_HH__

#include <deque>
#include <queue>

#include "cpu/vp/value_predictor.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "base/types.hh"
#include "base/cache/associative_cache.hh"
#include "params/StrideAvppLVP.hh"
#include "cpu/random/lsfr16.hh"
#include "cpu/o3/lsq_unit.hh"
#include "mem/port.hh"

namespace gem5 {

    /** Represents the result of the address prediction */
    struct ATResult {
        Addr predictedAddress;
        Addr prefetchAddress;
        bool predict;
    };

    /** Represents sender state when performing prefetch operations */
    struct PrefetchSenderState : public Packet::SenderState {
        ThreadID tid;
        InstSeqNum seqNum;

        PrefetchSenderState(ThreadID tid, InstSeqNum seqNum) : tid(tid), seqNum(seqNum) {}
    };

    class StrideAvppLVP : public ValuePredictor {
        public:
            StrideAvppLVP(const StrideAvppLVPParams &params);

            VPResult lookup(ThreadID tid, Addr inst_addr, InstSeqNum seq_num) override;
            void updateWhenLoad(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
                Addr load_address, RegVal correct_val, RegVal predicted_val,
                bool value_predicted, Cycles rn_to_ex_delay) override;
            void updateWhenStore(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
                Addr store_address, uint8_t *data_written,
                unsigned effective_size, ByteOrder guest_byte_order) override;
            void squash(const InstSeqNum seq_num) override;

            Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;

        private:

            const StrideAvppLVPParams &params;

            class PrefetchRequestPort : public RequestPort {
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

            /** Stores blocked prefetch requests by the memory system. */
            std::queue<PacketPtr> blockedPrefetchRequests;

            //There are two tables, so two kinds of entries.
            //AT (Address Table) -> ATEntry
            //VT (Value Table) -> VTEntry

            struct ATEntry : public TaggedEntry {

                ATEntry(TagExtractor ext) : TaggedEntry(), pdis(0), d(1), valid(false), tid(0), confidence(0) {
                    registerTagExtractor(ext);
                }

                /** Prefetch distance */
                uint8_t pdis;

                /** Prefetch Update Direction. 0 = decrease direction. 1 = increase direction */
                bool d; //Called "d" because the paper calls it p-bit.

                /** Whether or not the entry is valid. */
                bool valid;

                /** The entry's thread id. */
                ThreadID tid;

                /** Confidence of the prediction */
                int confidence;

                //PREDICTION:

                /** Address predicted */
                Addr predictedAddress;

                /** The stride*/
                int64_t stride;
            };

            struct VTEntry : public TaggedEntry {

                //Sets the extractor right away
                VTEntry(TagExtractor ext) : TaggedEntry(), valid(false), tid(0) {
                    registerTagExtractor(ext);
                }

                /** Whether or not the entry is valid. */
                bool valid;

                /** The entry's thread id. */
                ThreadID tid;

                /** Value predicted */
                uint64_t value;
            };

            /** AT table */
            AssociativeCache<ATEntry> addressTable; //IN THIS CONTEXT: this is a stride predictor -> same PC = stride load address.

            /** VT table */
            AssociativeCache<VTEntry> valueTable;

            /** Random number generator, for calculating prefetch distance update */
            LFSR16 lfsr16;

            /** Issues a prefetch load for a certain address. */
            void issuePrefetchLoad(ThreadID tid, InstSeqNum seqNum, Addr prefetchAddress);

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
            struct StrideAvppLVPStats : public statistics::Group {
                StrideAvppLVPStats(statistics::Group *parent);

                statistics::Scalar constantCorrect;
                statistics::Scalar strideCorrect;
                statistics::Scalar numZeroConstLoads;
                statistics::Scalar numOneConstLoads;

                statistics::Distribution valuePredSavedCyclesLog2;
                statistics::Distribution valuePredSavedCycles;
            } lvpstats;
    };

} //namespace gem5

#endif //__CPU_VP_STRIDE_AVPP_LVP_HH__
