/*
 * Copyright (c) 2014 ARM Limited
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

#include "cpu/champsim_trace.hh"

#include "base/callback.hh"
#include "base/output.hh"

// #include "config/the_isa.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/ExecEnable.hh"
#include "proto/inst.pb.h"
#include "sim/core.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace trace {

// ProtoOutputStream *ChampSimTrace::traceStream;

#define REG_SP		6
#define REG_IP		26
#define REG_FLAGS	25
#define REG_AX		56



void
ChampSimTraceRecord::dump()
{
    // We're trying to build an instruction trace so we just want macro-ops and
    // instructions that aren't macro-oped
    if (macroStaticInst && staticInst->isLastMicroop()) {
        tracer.traceInst(staticInst, *this);
    }
    if (!staticInst->isMicroop()) {
        tracer.traceInst(staticInst, *this);
    }
}

ChampSimTrace::ChampSimTrace(const ChampSimTraceParams &p)
    : InstTracer(p), traceStream(nullptr), curInst(nullptr)
{
    // Create our output file
    createTraceFile(p.file_name);
}

void
ChampSimTrace::createTraceFile(std::string filename)
{
    // Since there is only one output file for all tracers check if it exists
    if (traceStream)
        return;



    // std::string fn = simout.resolve(filename);

    // traceStream = fopen(fn.c_str(), "ab");
    // if (!traceStream){
    //     panic("Failed to open trace file %s\n", filename);
    // }
    traceStream = simout.findOrCreate(filename, true)->stream();


    // get a callback when we exit so we can close the file
    registerExitCallback([this]() { closeStreams(); });
}


void
ChampSimTrace::writeInst()
{
    // Write the instruction to the file
    // fwrite(curInst, sizeof(ChampSimInstFormat), 1, traceStream);
    traceStream->write(reinterpret_cast<char*>(curInst),
                       sizeof(ChampSimInstFormat));
    delete curInst;
    curInst = NULL;
}

void
ChampSimTrace::closeStreams()
{
    if (curInst) {
        writeInst();
    }

    if (!traceStream)
        return;

    // fclose(traceStream);
    delete traceStream;
    traceStream = NULL;
}

ChampSimTrace::~ChampSimTrace()
{
    closeStreams();
}

ChampSimTraceRecord*
ChampSimTrace::getInstRecord(Tick when, ThreadContext *tc,
                             const StaticInstPtr si,
                             const PCStateBase &pc, const StaticInstPtr mi)
{
    // // Only record the trace if Exec debugging is enabled
    // if (!debug::ExecEnable)
    //     return NULL;

    return new ChampSimTraceRecord(*this, when, tc, si, pc, mi);

}

void
ChampSimTrace::traceInst(StaticInstPtr si, ChampSimTraceRecord &rec)
{
    if (curInst) {
        //TODO if we are running multi-threaded I assume we'd need a lock here
        writeInst();

    }


    // Create a new instruction message and fill out the fields
    curInst = new ChampSimInstFormat();

    auto tc = rec.getThread();

    // Handle a branch
    curInst->ip = tc->pcState().instAddr();
    if (si->isControl()) {
        traceBranch(si, tc->pcState().branching());
    } else {

    // std::cout << " NDest: " << si->numDestRegs();
    // std::cout << " [";


    // Get the registers
    auto max = (si->numDestRegs() < NUM_INSTR_DESTINATIONS)
            ? si->numDestRegs() : NUM_INSTR_DESTINATIONS;

    for (int i = 0; i < max; i++) {

        auto x = si->destRegIdx(i).index();
        if (x == REG_IP) x = 64;
        if (x == REG_SP) x = 65;
        if (x == REG_FLAGS) x = 66;
        if (x == 0) x = 67;
        curInst->destination_registers[i] = x;
        // std::cout << " " << x;
    }


    // std::cout << "] NSrc: " << si->numSrcRegs() << " [";
    max = (si->numSrcRegs() < NUM_INSTR_SOURCES)
            ? si->numSrcRegs() : NUM_INSTR_SOURCES;

    for (int i = 0; i < max; i++) {
        auto x = si->srcRegIdx(i).index();
        if (x == REG_IP) x = 64;
        if (x == REG_SP) x = 65;
        if (x == REG_FLAGS) x = 66;
        if (x == 0) x = 67;
        curInst->source_registers[i] = x;
        // std::cout << " " << x;
    }

    // std::cout << "]";

    // For memory operations get the source or destination addresses
    if (rec.getMemValid()) {
        if (si->isLoad()) {
            curInst->source_memory[0] = rec.getAddr();
        } else if (si->isStore()) {
            curInst->destination_memory[0] = rec.getAddr();
        }
    }
    }

    // std::cout << std::endl;

}




void
ChampSimTrace::traceMem(StaticInstPtr si, Addr a, Addr s, unsigned f)
{
    panic_if(!curInst, "Memory access w/o msg?!");


}

void
ChampSimTrace::traceBranch(StaticInstPtr si, bool taken)
{
    typedef enums::BranchType BranchType;

    auto br_type = branch_prediction::getBranchType(si);

    assert(si->isControl());
    curInst->is_branch = true;

    switch (br_type) {
    case BranchType::DirectUncond:
        // writes IP only
        curInst->destination_registers[0] = REG_IP;
        curInst->branch_taken = taken;
        break;
    case BranchType::DirectCond:
        curInst->branch_taken = taken;
        // reads FLAGS, writes IP
        curInst->destination_registers[0] = REG_IP;
        // turns out pin records conditional direct branches as also reading
        // IP. whatever.
        curInst->source_registers[0] = REG_IP;
        curInst->source_registers[1] = REG_FLAGS;
        break;
    case BranchType::CallIndirect:
        curInst->branch_taken = true;
        // reads something else, reads IP, reads SP, writes SP, writes IP
        curInst->destination_registers[0] = REG_IP;
        curInst->destination_registers[1] = REG_SP;
        curInst->source_registers[0] = REG_IP;
        curInst->source_registers[1] = REG_SP;
        curInst->source_registers[2] = REG_AX;
        break;
    case BranchType::CallDirect:
        curInst->branch_taken = true;
        // reads IP, reads SP, writes SP, writes IP
        curInst->destination_registers[0] = REG_IP;
        curInst->destination_registers[1] = REG_SP;
        curInst->source_registers[0] = REG_IP;
        curInst->source_registers[1] = REG_SP;
        break;
    case BranchType::IndirectUncond:
        curInst->branch_taken = true;
        // reads something else, writes IP
        curInst->destination_registers[0] = REG_IP;
        curInst->source_registers[0] = REG_AX;
        break;
    case BranchType::Return:
        curInst->branch_taken = true;
        // reads SP, writes SP, writes IP
        curInst->source_registers[0] = REG_SP;
        curInst->destination_registers[0] = REG_IP;
        curInst->destination_registers[1] = REG_SP;
        break;
    default:
        break;
    }

    // std::cout << " Branch: " << br_type << " Taken: " << taken;

}



} // namespace Trace
} // namespace gem5
