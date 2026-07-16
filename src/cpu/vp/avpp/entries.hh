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

#ifndef __CPU_AVPP_ENTRIES_HH__
#define __CPU_AVPP_ENTRIES_HH__

#include "base/types.hh"
#include "mem/cache/tags/tagged_entry.hh"

namespace gem5::avpp
{

//There are two tables, so two kinds of entries.
//AT (Address Table) -> ATEntry
//VT (Value Table) -> VTEntry

struct ATEntry : public TaggedEntry
{

    ATEntry(TagExtractor ext) : TaggedEntry(), pdis(0), d(1),
    valid(false), tid(0), confidence(0)
    {
        registerTagExtractor(ext);
    }

    /** Prefetch distance */
    uint8_t pdis;

    /** Prefetch Update Direction. 0 = decrease direction.
     * 1 = increase direction */
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

struct VTEntry : public TaggedEntry
{

    //Sets the extractor right away
    VTEntry(TagExtractor ext) : TaggedEntry(), valid(false), tid(0)
    {
        registerTagExtractor(ext);
    }

    /** Whether or not the entry is valid. */
    bool valid;

    /** The entry's thread id. */
    ThreadID tid;

    /** Value predicted */
    uint64_t value;
};

struct ATResult
{
    Addr predictedAddress;
    Addr prefetchAddress;
    bool predict;
};

} //namespace gem5::avpp

#endif //__CPU_AVPP_ENTRIES_HH__
