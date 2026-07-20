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

#ifndef __CPU_AVPP_INJECT_FETCHER_HH__
#define __CPU_AVPP_INJECT_FETCHER_HH__

#include "arch/generic/mmu.hh"
#include "base/statistics.hh"

//Forward declaration
namespace gem5::o3 {
  class CPU;
}

namespace gem5::avpp::fetchers
{

/** Represents a prefetch request
 * It handles the translation basically for free. */
struct PrefetchRequest : public BaseMMU::Translation
{
    PrefetchRequest(gem5::o3::CPU *cpu, RequestorID requestorID, Addr vaddr,
                    ThreadID tid, InstSeqNum seqNum,
                    std::function<void(PrefetchRequestPtr, const Fault&)> callback);
    ~PrefetchRequest();

    /** Pointer to the CPU */
    gem5::o3::CPU *cpu;

    /** Requestor ID */
    RequestorID requestorID;

    /** callback function */
    std::function<void(PrefetchRequestPtr, const Fault&)> callback;

    /** The virtual address */
    const Addr vaddr;

    /** The thread ID number */
    ThreadID tid;

    /** seqNum of the instruction making the request */
    InstSeqNum seqNum;

    /** The request that will be sent */
    RequestPtr req;
    /** The packet that will be sent */
    PacketPtr pkt;

    /** Creates the packet that is send to memory */
    void createPkt();

    void finish(const Fault &fault, const RequestPtr &finishedReq,
                    ThreadContext *tc, BaseMMU::Mode mode) override;

    /** Issues the translation request */
    void startTranslation();

    /** Marks that it is delayed. Literally DON'T CARE. */
    void
    markDelayed() override
    {}
};

using PrefetchRequestPtr = PrefetchRequest*;

struct PrefetchSenderState : public Packet::SenderState
{
    PrefetchRequestPtr prefetchRequest;

    PrefetchSenderState(PrefetchRequestPtr prefetchRequest)
        : prefetchRequest(prefetchRequest) {}
};

} //namespace gem5::avpp::fetchers
#endif //__CPU_AVPP_INJECT_FETCHER_HH__
