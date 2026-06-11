#include "mem/cache/prefetch/berti.hh"
 
#include "base/output.hh"
#include "mem/cache/base.hh"
// associative_set_impl.hh is no longer needed: AssociativeCache is not a
// header-only template and has no separate *_impl.hh to include.
 
namespace gem5
{
namespace prefetch
{
 
BertiPrefetcher::BertiStats::BertiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(trainOnHit, statistics::units::Count::get(), "Count of train on hit"),
      ADD_STAT(trainOnMiss, statistics::units::Count::get(), "Count of train on miss"),
      ADD_STAT(notifySkippedCond1, statistics::units::Count::get(), "Count of notify skipped on mixed condition"),
      ADD_STAT(notifySkippedIsPF, statistics::units::Count::get(), "Count of notify skipped isPF"),
      ADD_STAT(notifySkippedNoEntry, statistics::units::Count::get(), "Count of notify skipped no Berti entry"),
      ADD_STAT(entryEvicts, statistics::units::Count::get(), "Count of Berti entry evicted")
{
}
 
BertiPrefetcher::BertiPrefetcher(const BertiPrefetcherParams &p)
    : Queued(p),
      maxAddrListSize(p.addrlist_size),
      maxDeltaListSize(p.deltalist_size),
      maxDeltafound(p.max_deltafound),
      //
      // Key differences from the AssociativeSet constructor:
      //
      //   AssociativeSet  (assoc, num_entries, idx_policy, rpl_policy, init)
      //   AssociativeCache(name,  num_entries, assoc, rpl_policy, idx_policy, init)
      //
      // AssociativeCache is a Named object, so a name string comes first.
      // The replacement policy and indexing policy arguments are swapped.
      //
      // The HistoryTableEntry init_val must carry a TagExtractor that matches
      // the indexing policy, because CacheEntry::insert(addr) extracts the tag
      // internally via that functor rather than accepting a pre-extracted tag.
      // We capture the indexing policy pointer in a lambda for this purpose.
      //
      historyTable(
          "berti.historyTable",
          p.history_table_entries,
          p.history_table_assoc,
          p.history_table_replacement_policy,
          p.history_table_indexing_policy,
          HistoryTableEntry(
              maxDeltaListSize,
              [ip = p.history_table_indexing_policy](Addr a) {
                  return ip->extractTag(a);
              })),
      hitSearchLatency(0),
      aggressivePF(p.aggressive_pf),
      useByteAddr(p.use_byte_addr),
      triggerPht(p.trigger_pht),
      statsBerti(this),
      trainBlockFilter(8),
      dumpTopDeltas(p.dump_top_deltas),
      // Default filter to the internal trainBlockFilter.  An outer
      // hierarchical prefetcher may redirect this to a wider shared filter.
      filter(&trainBlockFilter)
{
    registerExitCallback([this]() {
        if (this->dumpTopDeltas) {
            std::vector<std::pair<int64_t, uint64_t>> top_delta_vec;
            for (const auto &entry: topDeltas) {
                top_delta_vec.push_back(entry);
            }
            std::sort(top_delta_vec.begin(), top_delta_vec.end(),
                      [](const std::pair<int64_t, uint64_t> &a, const std::pair<int64_t, uint64_t> &b) {
                          return a.second > b.second;
                      });
 
            auto out_handle = simout.create("topBertiDeltas.txt", false, true);
            *out_handle->stream() << "Delta" << " " << "count" << std::endl;
            unsigned count = 0;
            for (const auto &it: top_delta_vec) {
                *out_handle->stream() << it.first << " " << it.second << std::endl;
                count++;
                if (count >= 1000) {
                    break;
                }
            }
 
            simout.close(out_handle);
 
            out_handle = simout.create("topBertiEvictDeltas.txt", false, true);
            *out_handle->stream() << "EvcitedDelta" << " " << "count" << std::endl;
            count = 0;
            for (const auto &it: evictedDeltas) {
                *out_handle->stream() << it.first << " " << it.second << std::endl;
                count++;
                if (count >= 100) {
                    break;
                }
            }
 
            simout.close(out_handle);
        }
    });
}
 
BertiPrefetcher::HistoryTableEntry*
BertiPrefetcher::updateHistoryTable(const PrefetchInfo &pfi)
{
    Addr training_addr = useByteAddr ? pfi.getAddr() : blockIndex(pfi.getAddr());
    //
    // AssociativeCache::findEntry takes only a single KeyType (Addr) key;
    // there is no is_secure overload.  The secure bit is checked explicitly
    // below via entry->secure after the tag-based lookup.
    //
    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pfi.getPC()));
    HistoryInfo new_info = {
        .vAddr = training_addr,
        .timestamp = curCycle()
    };
 
    if (entry) {
        // Secondary secure check: treat a secure/non-secure mismatch as a
        // miss so that the two namespaces remain logically separate.
        if (entry->secure != pfi.isSecure()) {
            entry = nullptr;
        }
    }
 
    if (entry) {
        historyTable.accessEntry(entry);
        DPRINTF(HWPrefetch, "PC=%lx, history table hit, Addr=%lx\n", pfi.getPC(), training_addr);
 
        bool found_addr_in_hist =
            std::find(entry->history.begin(), entry->history.end(), new_info) != entry->history.end();
        if (!found_addr_in_hist) {
            if (entry->history.size() >= maxAddrListSize) {
                entry->history.erase(entry->history.begin());
            }
            entry->history.push_back(new_info);
            entry->hysteresis = true;
            return entry;
        } else {
            DPRINTF(HWPrefetch, "PC=%lx, addr %lx found in history table hit, ignore\n", pfi.getPC(),
                    training_addr);
            return nullptr;  // ignore redundant req
        }
    } else {
        DPRINTF(HWPrefetch, "PC=%lx, history table miss\n", pfi.getPC());
        //
        // AssociativeCache::findVictim invalidates the victim internally
        // (same behaviour as the fork's AssociativeSet::findVictim), so the
        // entry is guaranteed to be invalid when we re-insert it below.
        //
        entry = historyTable.findVictim(pcHash(pfi.getPC()));
        if (entry->hysteresis) {
            entry->hysteresis = false;
            //
            // Hysteresis path: re-insert the evicted entry at its *original*
            // PC without resetting replacement data.  AssociativeCache has no
            // with_reset=false overload, so we bypass insertEntry() and call
            // CacheEntry::insert() directly.  This is safe because findVictim
            // already invalidated the entry (CacheEntry::setValid asserts
            // !isValid()).  The secure flag and all other fields are left
            // unchanged.
            //
            entry->insert(pcHash(entry->pc));
            // entry->secure intentionally unchanged
            // replacement data intentionally NOT reset (hysteresis semantics)
        } else {
            if (entry->bestDelta.status != NO_PREF) {
                int64_t blk_delta =
                    (int64_t)blockIndex(pfi.getAddr() + entry->bestDelta.delta) - blockIndex(pfi.getAddr());
                if (!useByteAddr) {
                    evictedBestDelta = entry->bestDelta.delta;
                } else {
                    evictedBestDelta = blk_delta;
                }
                statsBerti.entryEvicts++;
                evictedDeltas[blk_delta] = evictedDeltas.count(blk_delta) ? evictedDeltas[blk_delta] + 1 : 1;
            }
            // only when hysteresis is false
            entry->pc = pfi.getPC();
            entry->history.clear();
            entry->history.push_back(new_info);
            //
            // AssociativeCache::insertEntry(key, entry) calls entry->insert(key)
            // (which extracts the tag via the stored TagExtractor functor and
            // marks the entry valid) then resets replacement data.  The secure
            // flag is not part of the upstream API so we set it manually after
            // the call.
            //
            historyTable.insertEntry(pcHash(pfi.getPC()), entry);
            entry->secure = pfi.isSecure();
        }
    }
    return nullptr;
}
 
 
void BertiPrefetcher::searchTimelyDeltas(
    HistoryTableEntry &entry,
    const Cycles &search_latency,
    const Cycles &demand_cycle,
    const Addr &trigger_addr)
{
    DPRINTF(HWPrefetch, "latency: %lu, demand_cycle: %lu, history count: %lu\n", search_latency, demand_cycle,
            entry.history.size());
    std::list<int64_t> new_deltas;
    int delta_thres = useByteAddr ? blkSize : 8;
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        int64_t delta = trigger_addr - it->vAddr;
        DPRINTF(HWPrefetch, "delta (%x - %x) = %ld\n", trigger_addr, it->vAddr, delta);
 
        // skip short deltas
        if (labs(delta) <= delta_thres) {
            continue;
        }
 
        // if not timely, skip and continue
        if (it->timestamp + search_latency >= demand_cycle) {
            DPRINTF(HWPrefetch, "skip untimely delta: %lu + %lu <= %u : %ld\n", it->timestamp, search_latency,
                    demand_cycle, delta);
            continue;
        }
        assert(delta != 0);
        new_deltas.push_back(delta);
        DPRINTF(HWPrefetch, "Timely delta found: %d=(%x - %x)\n", delta, trigger_addr, it->vAddr);
        if (new_deltas.size() >= maxDeltafound) {
            break;
        }
    }
 
    entry.counter++;
 
    for (auto &delta : new_deltas) {
        bool miss = true;
        for (auto &delta_info : entry.deltas) {
            if (delta_info.coverageCounter != 0 && delta_info.delta == delta) {
                delta_info.coverageCounter++;
                DPRINTF(HWPrefetch, "Inc coverage for delta %d, cov = %d\n", delta, delta_info.coverageCounter);
                miss = false;
                break;
            }
        }
        // miss
        if (miss) {
            // find the smallest coverage and replace
            int replace_idx = 0;
            for (auto i = 0; i < entry.deltas.size(); i++) {
                if (entry.deltas[replace_idx].coverageCounter >= entry.deltas[i].coverageCounter) {
                    replace_idx = i;
                }
            }
            entry.deltas[replace_idx].delta = delta;
            entry.deltas[replace_idx].coverageCounter = 1;
            entry.deltas[replace_idx].status = NO_PREF;
            DPRINTF(HWPrefetch, "Add new delta: %d with cov = 1\n", delta);
        }
    }
 
    if (entry.counter >= 6) {
        entry.updateStatus();
        if (entry.counter >= 16) {
            entry.resetConfidence(false);
        }
    }
    printDeltaTableEntry(entry);
}
 
void
BertiPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses,
                                   const CacheAccessor &cache)
{
    // Upstream Queued::notify calls this signature.  Delegate to the
    // internal implementation; local_delta_pf_addr is only used by
    // fork-specific hierarchical prefetchers and is discarded here.
    Addr local_delta_pf_addr = 0;
    calculatePrefetchImpl(pfi, addresses, local_delta_pf_addr);
}
 
void
BertiPrefetcher::calculatePrefetchImpl(const PrefetchInfo &pfi,
                                       std::vector<AddrPriority> &addresses,
                                       Addr &local_delta_pf_addr)
{
    // reset learned delta
    evictedBestDelta = 0;
    lastUsedBestDelta = 0;
 
    DPRINTF(HWPrefetch,
            "Train prefetcher, pc: %lx, addr: %lx miss: %d last lat: [%d]\n",
            pfi.getPC(), blockAddress(pfi.getAddr()),
            pfi.isCacheMiss(), hitSearchLatency);
 
    trainBlockFilter.insert(blockIndex(pfi.getAddr()), 0);
 
    if (!pfi.isCacheMiss()) {
        HistoryTableEntry *hist_entry = historyTable.findEntry(pcHash(pfi.getPC()));
        if (hist_entry) {
            searchTimelyDeltas(*hist_entry, hitSearchLatency, curCycle(),
                               useByteAddr ? pfi.getAddr() : blockIndex(pfi.getAddr()));
            statsBerti.trainOnHit++;
        }
    }
 
    /** 1.train: update history table and compute learned delta*/
    auto entry = updateHistoryTable(pfi);
 
    /** 2.prefetch: search table of deltas, issue prefetch request */
    if (entry) {
        DPRINTF(HWPrefetch, "Delta table hit, pc: %lx\n", pfi.getPC());
        if (aggressivePF) {
            for (auto &delta_info : entry->deltas) {
                if (delta_info.status != NO_PREF) {
                    DPRINTF(HWPrefetch, "Using delta %d to prefetch\n", delta_info.delta);
                    int64_t delta = delta_info.delta;
                    Addr pf_addr =
                        useByteAddr ? pfi.getAddr() + delta : (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
                    sendPFWithFilter(pfi, pf_addr, addresses, 32,
                                     delta == entry->bestDelta.delta &&
                                     entry->bestDelta.coverageCounter >= 8);
                }
            }
        } else {
            if (entry->bestDelta.status != NO_PREF) {
                DPRINTF(HWPrefetch, "Using best delta %d to prefetch\n", entry->bestDelta.delta);
                Addr pf_addr = useByteAddr ? pfi.getAddr() + entry->bestDelta.delta
                                           : (blockIndex(pfi.getAddr()) + entry->bestDelta.delta) << lBlkSize;
                sendPFWithFilter(pfi, pf_addr, addresses, 32,
                                 entry->bestDelta.coverageCounter >= 8);
                if (triggerPht && entry->bestDelta.coverageCounter > 5) {
                    local_delta_pf_addr = pf_addr;
                }
            }
        }
    }
}
 
bool
BertiPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                                  std::vector<AddrPriority> &addresses,
                                  int prio,
                                  bool using_best_delta_and_confident)
{
    if (using_best_delta_and_confident) {
        lastUsedBestDelta = blockIndex(addr) - blockIndex(pfi.getAddr());
    }
 
    if (filter->contains(addr)) {
        DPRINTF(HWPrefetch, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        int64_t blk_delta = (int64_t)blockIndex(addr) - blockIndex(pfi.getAddr());
        topDeltas[blk_delta] = topDeltas.count(blk_delta) ? topDeltas[blk_delta] + 1 : 1;
        DPRINTF(HWPrefetch, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        // Upstream AddrPriority is std::pair<Addr, int32_t>.
        addresses.push_back(AddrPriority(addr, prio));
        return true;
    }
}
 
 
void
BertiPrefetcher::notifyFill(const CacheAccessProbeArg &acc)
{
    // Upstream Base::notifyFill passes a CacheAccessProbeArg; extract the
    // packet from it so the rest of the function can use pkt directly.
    const PacketPtr pkt = acc.pkt;
 
    if (pkt->req->isInstFetch() ||
        !pkt->req->hasVaddr() || !pkt->req->hasPC()) {
        DPRINTF(HWPrefetch, "Skip packet: %s\n", pkt->print());
        statsBerti.notifySkippedCond1++;
        return;
    }
 
    DPRINTF(HWPrefetch,
            "Cache Fill: %s isPF: %d, pc: %lx\n",
            pkt->print(), pkt->req->isPrefetch(), pkt->req->getPC());
 
    if (pkt->req->isPrefetch()) {
        statsBerti.notifySkippedIsPF++;
        return;
    }
 
    // fill latency
    Cycles miss_refill_search_lat = Cycles(0);
    hitSearchLatency = Cycles(0);
 
    //
    // AssociativeCache::findEntry takes only a KeyType (Addr); there is no
    // is_secure overload.  After the lookup we verify the secure flag
    // explicitly, matching the behaviour of the original TaggedEntry-based
    // findEntry(addr, is_secure).
    //
    // pkt->isSecure() is the standard gem5 accessor (used in PrefetchInfo
    // construction in base.cc) and is preferred over pkt->req->isSecure().
    //
    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pkt->req->getPC()));
    if (!entry || entry->secure != pkt->isSecure()) {
        statsBerti.notifySkippedNoEntry++;
        return;
    }
 
    /** Search history table, find deltas. */
    Cycles demand_cycle = ticksToCycles(pkt->req->time());
 
    DPRINTF(HWPrefetch, "Search delta for PC %lx\n", pkt->req->getPC());
    searchTimelyDeltas(*entry, miss_refill_search_lat, demand_cycle,
                       useByteAddr ? pkt->req->getVaddr() : blockIndex(pkt->req->getVaddr()));
    statsBerti.trainOnMiss++;
 
    DPRINTF(HWPrefetch, "Updating table of deltas, latency [%d]\n",
            miss_refill_search_lat);
}
 
}
}
