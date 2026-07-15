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

#ifndef __CPU_VP_STRIDE_LVP_HH__
#define __CPU_VP_STRIDE_LVP_HH__

#include <deque>

#include "base/cache/associative_cache.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/vp/value_predictor.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "params/StrideLVP.hh"

namespace gem5
{

class StrideLVP : public ValuePredictor
{
  public:
    StrideLVP(const StrideLVPParams &params);

    VPResult lookup(ThreadID tid, Addr inst_addr, InstSeqNum seq_num) override;
    void updateWhenLoad(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
                Addr load_address, RegVal correct_val, RegVal predicted_val,
                bool value_predicted, Cycles rn_to_ex_delay) override;
    void squash(const InstSeqNum seq_num) override;

  private:
    /** Load value predictor table entry */
    struct LVPEntry : public TaggedEntry
    {
        LVPEntry(TagExtractor ext)
            : TaggedEntry(), tag(0), tid(0), valid(false), confidence(0)
        {
            registerTagExtractor(ext);
        }

        /** The entry's tag. */
        Addr tag;

        /** The entry's thread id. */
        ThreadID tid;

        /** Whether or not the entry is valid. */
        bool valid;

        /** Confidence of the prediction */
        int confidence;

        /** Prediction */
        int64_t stride;

        uint64_t value;
        uint64_t instance_count;
    };
    /** Prediction table */
    AssociativeCache<LVPEntry> vpTable;

    struct InflightInfo
    {
        Addr iaddr;
        InstSeqNum sn;
    };
    // Track inflight prediction
    std::deque<InflightInfo> inflightPred;

    /** Internal call to find an address in the prediction table
     * @param inst_addr The loads entry
     * @return Returns a pointer to the entry if found, nullptr otherwise.
     */
    LVPEntry *findEntry(ThreadID tid, Addr inst_addr);

    Addr index(Addr inst_addr);
    LVPEntry::KeyType index(ThreadID tid, Addr inst_addr);

    unsigned numInflights(Addr iaddr);

    /** The confidence threshold */
    const int confThreshold;

    /** Reset policy. Reset to zero or decrement */
    const bool confResetToZero;

    const bool useStride;

    struct StrideLVPStats : public statistics::Group
    {
        StrideLVPStats(statistics::Group *parent);

        statistics::Scalar constantCorrect;
        statistics::Scalar strideCorrect;
        statistics::Scalar numZeroConstLoads;
        statistics::Scalar numOneConstLoads;

        statistics::Distribution valuePredSavedCyclesLog2;
        statistics::Distribution valuePredSavedCycles;
    } lvpstats;
};

} // namespace gem5

#endif // __CPU_VP_STRIDE_LVP_HH__
