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

#ifndef __CPU_CHAMPSIM_TRACE_HH__
#define __CPU_CHAMPSIM_TRACE_HH__

#include <fstream>

#include "arch/generic/pcstate.hh"
#include "base/output.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "params/ChampSimTrace.hh"
#include "sim/insttracer.hh"

namespace gem5
{

class ThreadContext;

namespace trace {

/**
 * This in an instruction tracer that records the flow of instructions through
 * multiple cpus and systems to a protobuf file specified by proto/inst.proto
 * for further analysis.
 */


#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4


struct ChampSimInstFormat
{
    unsigned long long int ip = 0;  // instruction pointer (program counter)

    unsigned char is_branch = 0;    // is this branch
    unsigned char branch_taken = 0; // if so, is this taken

    // in/out registers
    unsigned char destination_registers[NUM_INSTR_DESTINATIONS] = {0};
    unsigned char source_registers[NUM_INSTR_SOURCES] = {0};

    // in/out memory
    unsigned long long int destination_memory[NUM_INSTR_DESTINATIONS] = {0};
    unsigned long long int source_memory[NUM_INSTR_SOURCES] = {0};
};



class ChampSimTraceRecord : public InstRecord
{
  public:
    ChampSimTraceRecord(ChampSimTrace& _tracer, Tick when, ThreadContext *tc,
                      const StaticInstPtr si, const PCStateBase &pc,
                      const StaticInstPtr mi = NULL)
        : InstRecord(when, tc, si, pc, mi), tracer(_tracer)
    {}

    /** called by the cpu when the instruction commits.
     * This implementation of dump calls ChampSimTrace to output the contents
     * to champsim trace format
     */
    void dump() override;

  protected:
    ChampSimTrace& tracer;

};

class ChampSimTrace : public InstTracer
{
 public:
    ChampSimTrace(const ChampSimTraceParams &p);
    virtual ~ChampSimTrace();

    ChampSimTraceRecord* getInstRecord(Tick when, ThreadContext *tc, const
                                    StaticInstPtr si, const PCStateBase &pc,
                                    const StaticInstPtr mi = NULL) override;

  protected:

    /** One output stream for the entire simulation.
     * We encode the CPU & system ID so all we need is a single file
     */
    // FILE *traceStream;
    std::ostream *traceStream;


    /** This is the message were working on writing. The majority of the
     * message exists however the memory accesses will be delayed.
     */
    ChampSimInstFormat *curInst;

    /** Create the output file and write the header into it
     * @param filename the file to create (if ends with .gz it will be
     * compressed)
     */
    void createTraceFile(std::string filename);

    void writeInst();

    /** If there is a pending message still write it out and then close the
     * file
     */
    void closeStreams();

    /** Write an instruction to the trace file
     * @param si for the machInst and opClass
     * @param pc for the PC Addr
     */
    void traceInst(StaticInstPtr si, ChampSimTraceRecord &rec);

    /** Write a memory request to the trace file as part of the cur instruction
     * @param si for the machInst and opClass
     * @param a address of the request
     * @param s size of the request
     * @param f flags for the request
     */
    void traceMem(StaticInstPtr si, Addr a, Addr s, unsigned f);

    /** Write a branch to the trace file as part of the cur instruction
     * @param si for the machInst and opClass
     * @param pc for the PC Addr
     * @param taken if the branch was taken
     */
    void traceBranch(StaticInstPtr si, bool taken);

    friend class ChampSimTraceRecord;
};

} // namespace Trace
} // namespace gem5

#endif // __CPU_CHAMPSIM_TRACE_HH__
