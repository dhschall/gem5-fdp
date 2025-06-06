/*
 * Copyright (c) 2025 Technical University of Munich
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
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

#include "cpu/pred/llbp_ref.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Fetch.hh"
#include "cpu/pred/llbpref/llbp.h"

namespace gem5
{

namespace branch_prediction
{

LLBPRef::LLBPRef(const LLBPRefParams &params)
    : ConditionalPredictor(params),
      predictor(nullptr),
      stats(this)
{
    if (params.inf) {
        predictor = new LLBP::LLBPInfTageSCL64k();
    } else {
        predictor = new LLBP::LLBPTageSCL64k();
    }
}

LLBPRef::~LLBPRef()
{
    static_cast<LLBP::LLBP*>(predictor)->PrintStat(1.0);
    delete predictor;
}

void
LLBPRef::updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void * &bp_history)
{
// Place holder for a function that is called to update predictor history
}


Prediction
LLBPRef::lookup(ThreadID tid, Addr branch_addr, void * &bp_history)
{
    auto pc = branch_addr >> instShiftAmt;
    auto pred = predictor->GetPrediction(pc);
    return staticPrediction(pred);
}

void
LLBPRef::update(ThreadID tid, Addr branch_addr, bool taken, void *&bp_history,
                bool squashed, const StaticInstPtr & inst, Addr target)
{
    if (squashed) {
        return;
    }

    auto brtype = getBranchType(inst);
    OpType opType = OPTYPE_OP;
    switch (brtype) {
        case BranchType::DirectUncond:
            opType = OPTYPE_JMP_DIRECT_UNCOND;
            break;
        case BranchType::DirectCond:
            opType = OPTYPE_JMP_DIRECT_COND;
            break;
        case BranchType::IndirectUncond:
            opType = OPTYPE_JMP_INDIRECT_UNCOND;
            break;
        case BranchType::IndirectCond:
            opType = OPTYPE_JMP_INDIRECT_COND;
            break;
        case BranchType::CallDirect:
            opType = OPTYPE_CALL_DIRECT_UNCOND;
            break;
        case BranchType::CallIndirect:
            opType = OPTYPE_CALL_INDIRECT_UNCOND;
            break;
        case BranchType::Return:
            opType = OPTYPE_RET_UNCOND;
            break;
        default:
            opType = OPTYPE_OP;
            break;
    }

    if (opType == OPTYPE_OP) {
        return;
    }
    auto pc = branch_addr >> instShiftAmt;
    auto _target = target >> instShiftAmt;

    if (brtype == BranchType::DirectCond) {
        predictor->UpdatePredictor(pc, taken, false, _target);
    } else {
        predictor->TrackOtherInst(pc, opType, taken, _target);
    }




}

void
LLBPRef::LLBPStats::preDumpStats()
{
    // This function is called before the stats are dumped.
    // We can use it to print some additional information.
    // DPRINTF(LLBP, "LLBPRef: Pre-dump stats for predictor\n");
    parent->predictor->PrintStat(1.0);
}


} // namespace branch_prediction
} // namespace gem5
