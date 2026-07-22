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

#ifndef __CPU_ATOMIC_VALUE_PREDICTOR_HH__
#define __CPU_ATOMIC_VALUE_PREDICTOR_HH__

#include "base/statistics.hh"
#include "cpu/inst_seq.hh"
#include "cpu/vp/structs.hh"
#include "enums/ByteOrder.hh"
#include "params/AtomicValuePredictor.hh"
#include "sim/clocked_object.hh"

//Forward declaration
namespace gem5::o3 {
  class CPU;
}

namespace gem5
{

/** Abstract AtomicValuePredictor */
class AtomicValuePredictor : public SimObject
{
  public:
    AtomicValuePredictor(const AtomicValuePredictorParams &params);

    /** Sets the CPU that the value predictor is in. */
    void setO3CPU(gem5::o3::CPU *cpu);

    /**
     * Looks up the given instruction address and returns
     * a LvptResult with the LctResult and predicted value.
     * @param inst_addr The address of the instruction to look up.
     * @param bp_history Pointer to any bp history state.
     * @return Whether or not the branch is taken.
     */
    // GETS CALLED DURING FETCH
    virtual VPResult lookup(ThreadID tid, Addr inst_addr,
                            InstSeqNum seq_num) = 0;

    /** Updates the BTB with the target of a branch.
     *  @param inst_pc The address of the branch being updated.
     *  @param target_pc The target address of the branch.
     */
    // GETS CALLED DURING COMMIT
    virtual void updateWhenLoad(ThreadID tid, Addr inst_addr,
                        InstSeqNum seq_num, Addr load_address,
                        RegVal correct_val, RegVal predicted_val,
                        bool value_predicted, Cycles rn_to_ex_delay) = 0;

    // GETS CALLED DURING COMMIT
    virtual void updateWhenStore(ThreadID tid, Addr inst_addr,
                        InstSeqNum seq_num, Addr store_address,
                        const std::vector<uint8_t>& data_written, unsigned effective_size,
                        ByteOrder guest_byte_order) {};

    // If predict error, squash the inflight instructions in value predictor.
    // GETS (hopefully not) CALLED DURING COMMIT
    virtual void squash(const InstSeqNum seq_num) {};

  protected:
    /** Number of the threads for which the branch history is maintained. */
    const unsigned numThreads;

    /** Instruction shift amount */
    const unsigned instShiftAmt;

    /** Pointer to the O3 CPU used */
    gem5::o3::CPU *cpu;

    struct AtomicValuePredictorStats : public statistics::Group
    {
        AtomicValuePredictorStats(statistics::Group *parent);

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
};

} // namespace gem5

#endif // __CPU_ATOMIC_VALUE_PREDICTOR_HH__
