/*
 * Copyright (c) 2022-2023 The University of Edinburgh
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


#include "mem/cache/prefetch/eip.hh"

#include <utility>

#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/EntanglingPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "debug/CountTask.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

EntanglingPrefetcher::EntanglingPrefetcher(const EntanglingPrefetcherParams &p)
    : Base(p),
      index(p.index_assoc, p.index_entries, p.index_indexing_policy,
            p.index_replacement_policy),
      buffer(p.buffer_entries),
      listenersPC(),
      cpu(p.cpu),
      transFunctional(p.translate_functional),
      latency(cyclesToTicks(p.latency)),
      cacheSnoop(true), stats(this)
{
}

PacketPtr
EntanglingPrefetcher::getPacket()
{
    if (pfq.size() == 0)
        return nullptr;
    
    prefetchStats.pfIssued++;
    issuedPrefetches++;

    PacketPtr pkt = pfq.front().pkt;
    pfq.pop_front();
    return pkt;
}


void
EntanglingPrefetcher::PrefetchListenerPC::notify(const Addr& pc)
{
    parent.notifyRetiredInst(pc);
}

void
EntanglingPrefetcher::addEventProbeRetiredInsts(SimObject *obj, const char *name)
{
    ProbeManager *pm(obj->getProbeManager());
    listenersPC.push_back(new PrefetchListenerPC(*this, pm, name));
}

void
EntanglingPrefetcher::notifyRetiredInst(const Addr pc)
{
    all_insts++;
    const Addr pc_blk = pc >> lBlkSize;
    if (pc_blk == last_pc_blk) return;
    all_blks++;

    if (std::find(recent_blks.begin(), recent_blks.end(), pc_blk) == recent_blks.end()) {
        recent_blks.push_back(pc_blk);
        blk_time++;
    }
    if (recent_blks.size() > 20)
        recent_blks.pop_front();

    updateBB(pc_blk); // check buffer and update BB
    last_pc_blk = pc_blk;
} // retire


int EntanglingPrefetcher::get_max_dsts(BB *table_entry, uint64_t src_blk) {
    int max_dsts = 1;
    int max_diffbits = 0;
    for (int i = 0; i < table_entry->dsts.size(); i++) {
        uint64_t dst_blk = table_entry->dsts[i].addr;
        if (dst_blk == src_blk)
            continue;
        uint64_t diff = dst_blk ^ src_blk;
        int zeros = __builtin_clzll(diff);
        int diffbits = 64 - zeros;
        if (diffbits > max_diffbits) {
            max_diffbits = diffbits;
        }
    }
    if (max_diffbits <= 20)
        max_dsts = 2;
    if (max_diffbits <= 12)
        max_dsts = 3;
    if (max_diffbits <= 9)
        max_dsts = 4;
    return max_dsts;
}


// add basic blocks into entangled table and update history buffer
void EntanglingPrefetcher::updateBB(const Addr pc_blk) {
    // try merge to buffer
    bool found = false;
    for (auto &bb : buffer) {
        if (pc_blk >= bb.head_blk && pc_blk <= bb.tail_blk + 1) {
            if (pc_blk == bb.tail_blk + 1) { // is BB's next blk
                bb.tail_blk = pc_blk;
                // update size in entangled table
                BB *table_entry = index.findEntry(bb.head_blk, false);
                if (table_entry != nullptr) { // found in buffer
                    if (bb.tail_blk > table_entry->tail_blk) { // no overlap
                        BB *tmp_entry = index.findEntry(bb.tail_blk, false);
                        if (tmp_entry != nullptr)
                            tmp_entry->invalidate();
                        table_entry->tail_blk = bb.tail_blk;
                    }
                }
            }
            found = true;
            break;
        }
    }

    if (!found) {
        // not in buffer, new bb
        BB newbb(pc_blk, pc_blk, curTick(), blk_time);
        buffer.push_back(newbb);

        // whether current head-blk miss
        bool is_miss = false;
        Addr miss_baddr = 0;
        Tick miss_lat = 0;
        for (int i = lat_buffer.size() - 1; i >= 0; i--) {
            if (lat_buffer[i].first == pc_blk) {
                is_miss = true;
                miss_baddr = lat_buffer[i].first;
                miss_lat = lat_buffer[i].second;
                lat_buffer.erase(lat_buffer.begin(), lat_buffer.begin() + i + 1); // delete from recent
                break;
            }
        }

        if (is_miss) { // the new BB-head misses
            // miss, find the history BB to entangle
            BB *src_bb = nullptr;
            Tick biggest_tick = 0;
            uint64_t interval_blks = 0;
            for (auto &bb : buffer) {
                if (curTick() - bb.timestamp > miss_lat && bb.timestamp > biggest_tick) {
                    biggest_tick = bb.timestamp;
                    src_bb = &bb;
                    interval_blks = blk_time - bb.blk_timestamp; // ticks between src and dest
                }
            }
            if (src_bb != nullptr) {
                BB *table_entry = index.findEntry(src_bb->head_blk, false);
                if (table_entry != nullptr) { // found in buffer
                    if (table_entry->dsts.size() == 0 || table_entry->dsts.back().addr != newbb.head_blk) {
                        table_entry->dsts.push_back(Dst(newbb.head_blk, 3, interval_blks));

                        int max_dsts = get_max_dsts(table_entry, src_bb->head_blk);
                        while (table_entry->dsts.size() > max_dsts) {
                            auto min_conf_it = std::min_element(table_entry->dsts.begin(), table_entry->dsts.end(), 
                                [](const Dst& a, const Dst& b) { return a.conf < b.conf; });
                            table_entry->dsts.erase(min_conf_it);
                            max_dsts = get_max_dsts(table_entry, src_bb->head_blk);
                        }
                    }
                }
            }
        }

        // add new BB into entangled table
        BB *table_entry = index.findEntry(pc_blk, false);
        if (table_entry == nullptr) { // not found in table
            table_entry = index.findVictim(pc_blk);
            assert(table_entry != nullptr);
            index.insertEntry(pc_blk, false, table_entry);
            table_entry->head_blk = pc_blk;
            table_entry->tail_blk = pc_blk;
        }
    }
}


void
EntanglingPrefetcher::calculatePrefetch(const PrefetchInfo &pfi)
{
    if (!pfi.hasPC()) {
        return;
    }
    const Addr pc_blk = pfi.getPC() >> lBlkSize;
    // look in entangled table, generate pf reqs
    std::vector<Addr> pf_addr_vec;
    std::map<Addr, Addr> src_for_head;
    BB *src_entry = index.findEntry(pc_blk, false);
    if (src_entry != nullptr) { // found in buffer
        index.accessEntry(src_entry);

        // issue pf for srcBB
        for (Addr addr_blk = src_entry->head_blk + 1; addr_blk <= src_entry->tail_blk; addr_blk++) {
            Addr pf_addr = addr_blk << lBlkSize;
            pf_addr_vec.push_back(pf_addr);
        }
        for (int i = 0; i < src_entry->dsts.size(); i++) {
            // find for dst BB
            if (src_entry->dsts[i].conf > 0) { // confidence > 0
                BB *dst_entry = index.findEntry(src_entry->dsts[i].addr, false);
                if (dst_entry != nullptr) {
                    src_for_head[dst_entry->head_blk << lBlkSize] = src_entry->head_blk;
                    for (Addr addr_blk = dst_entry->head_blk; addr_blk <= dst_entry->tail_blk; addr_blk++) {
                        Addr pf_addr = addr_blk << lBlkSize;
                        pf_addr_vec.push_back(pf_addr);
                    }
                }
            }
        }
    }

    // push pf_addr_vec to pfq
    for (int i = 0; i < pf_addr_vec.size(); i++) {
        Addr vaddr = pf_addr_vec[i];
        PacketPtr pkt = createPrefetchPacket(vaddr, true);
        if (!pkt) continue;
        if (src_for_head.count(vaddr) > 0) { // src bb head issued this prefetch
            pkt->srcbb = src_for_head[vaddr];
        }

        if (cacheSnoop && (inCache(pkt->getAddr(), pkt->isSecure())
                    || (inMissQueue(pkt->getAddr(), pkt->isSecure())))) {
            stats.pfInCache++;
            delete pkt;
            continue;
        }

        Tick t = curTick() + latency;
        stats.pfCandidatesAdded++;
        pfq.push_back(PFQEntry(vaddr, pkt, t));
    }
} // access



PacketPtr
EntanglingPrefetcher::createPrefetchPacket(Addr addr, bool virtual_addr)
{
    /* Create a prefetch memory request */
    RequestPtr req = nullptr;
    Flags flags = Request::INST_FETCH|Request::PREFETCH;

    if (virtual_addr) {
        // The address is virtual -> we need translate first
        req = std::make_shared<Request>(
                addr, blkSize, flags, requestorId, addr, 0);


        // Translate the address from virtual to physical using
        // the functional translation function.
        // Note: This of course a hack and is not how it is in a real system.
        // Using functional tranlation underestimate the latency of the
        // translation and the page walk.
        // TODO: Add timing translation!
        if (!translateFunctional(req)) {
            return nullptr;
        }

    } else {
        // The adddress is physical -> no translation needed.
        req = std::make_shared<Request>(
                addr, blkSize, flags, requestorId);
    }

    req->taskId(context_switch_task_id::Prefetcher);
    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();

    return pkt;
}


bool
EntanglingPrefetcher::translateFunctional(RequestPtr req)
{
    if (mmu == nullptr) {
        return false;
    }

    auto tc = cache->system->threads[req->contextId()];

    DPRINTF(HWPrefetch, "%s Try trans of pc %#x\n",
                                mmu->name(), req->getVaddr());
    Fault fault = mmu->translateFunctional(req, tc, BaseMMU::Read);
    if (fault == NoFault) {
        DPRINTF(HWPrefetch, "%s Translation of vaddr %#x succeeded: "
                        "paddr %#x \n", mmu->name(), req->getVaddr(),
                        req->getPaddr());

        stats.translationSuccess++;
        return true;
    }
    stats.translationFail++;
    return false;
}



EntanglingPrefetcher::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(pfIdentified, statistics::units::Count::get(),
            "number of prefetches identified."),
    ADD_STAT(pfInCache, statistics::units::Count::get(),
            "number of prefetches hit in in cache"),
    ADD_STAT(pfCandidatesAdded, statistics::units::Count::get(),
            "Number of perfetch candidates added to the prefetch queue"),
    ADD_STAT(translationFail, statistics::units::Count::get(),
             "Number of prefetches that failed translation"),
    ADD_STAT(translationSuccess, statistics::units::Count::get(),
             "Number of prefetches that succeeded translation")
{
}

} // namespace prefetch
} // namespace gem5
