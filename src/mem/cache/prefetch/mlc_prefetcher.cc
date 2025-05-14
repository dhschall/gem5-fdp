/*
 * Copyright (c) 2020 SHIN
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
 *
 * Authors: Jongmin SHIN
 *
 */

#include "mem/cache/prefetch/mlc_prefetcher.hh"

#include <cassert>

#include "base/intmath.hh"
#include "cpu/base.hh"
#include "mem/cache/base.hh"
#include "base/trace.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/MlcPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

    namespace prefetch
    {

        MlcPrefetcher::MlcPrefetcher(const MlcPrefetcherParams &p)
            : Queued(p)
        {
            DPRINTF(AdaptiveDdioMlcPrefetcher, "MlcPrefetcher Contructor\n");
            ddioPrefetch = true;
        }

        void
        MlcPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
        {
            Queued::notify(pkt, pfi);
            DPRINTF(AdaptiveDdioMlcPrefetcher, "MlcPrefetcher notify was called. pkt %s\n", pkt->print());
        }

        void
        MlcPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                         std::vector<AddrPriority> &addresses)
        {

            // Get required packet info
            Addr pf_addr = pfi.getAddr();
            addresses.push_back(AddrPriority(pf_addr, 0));
        }

        PacketPtr
        MlcPrefetcher::getPacket()
        {
            DPRINTF(AdaptiveDdioMlcPrefetcher, "MlcPrefetcher get Packet was called\n");
            PacketPtr pkt = Queued::getPacket();
            DPRINTF(AdaptiveDdioMlcPrefetcher, "MlcPrefetcher get Packet %s\n", pkt->print());
            pkt->setBlockIO();
            return pkt;
        }

    }

    // prefetch::MlcPrefetcher*
    // MlcPrefetcherParams::create()
    // {
    //     return new prefetch::MlcPrefetcher(this);
    // }

}