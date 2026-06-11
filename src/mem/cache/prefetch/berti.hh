#ifndef __MEM_CACHE_PREFETCH_BERTI_HH__
#define __MEM_CACHE_PREFETCH_BERTI_HH__

#include <unordered_map>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/cache/associative_cache.hh"  // upstream: replaces associative_set.hh
#include "base/cache/cache_entry.hh"        // upstream: CacheEntry replaces TaggedEntry
#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/BertiPrefetcher.hh"

namespace gem5
{

struct BertiPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{

class BertiPrefetcher : public Queued
{

    int maxAddrListSize;
    int maxDeltaListSize;
    int maxDeltafound;

  protected:
    struct HistoryInfo
    {
        Addr vAddr{0};
        Cycles timestamp{0};

        bool operator == (const HistoryInfo &rhs) const
        {
            return vAddr == rhs.vAddr;
        }
    };

    enum DeltaStatus { L1_PREF, L2_PREF, NO_PREF };
    struct DeltaInfo
    {
        uint8_t coverageCounter = 0;
        int delta = 0;
        DeltaStatus status = NO_PREF;
    };

    class TableOfDeltasEntry
    {
      public:
        std::vector<DeltaInfo> deltas;
        uint8_t counter = 0;
        DeltaInfo bestDelta;

        void resetConfidence(bool reset_status)
        {
            counter = 0;
            for (auto &info : deltas) {
                info.coverageCounter = 0;
                if (reset_status) {
                    info.status = NO_PREF;
                }
            }
            if (reset_status) {
                bestDelta.delta = 0;
                bestDelta.status = NO_PREF;
            }
        }

        void updateStatus()
        {
            uint8_t max_cov = 0;
            for (auto &info : deltas) {
                info.status = (info.coverageCounter >= 2) ? L2_PREF : NO_PREF;
                info.status = (info.coverageCounter >= 4) ? L1_PREF : info.status;
                if (info.status != NO_PREF && info.coverageCounter > max_cov) {
                    max_cov = info.coverageCounter;
                    bestDelta = info;
                }
            }
            if (max_cov == 0) {
                bestDelta.delta = 0;
                bestDelta.status = NO_PREF;
            }
        }

        TableOfDeltasEntry(int size) {
            deltas.resize(size);
        }
    };

    /**
     * History table entry.
     *
     * Upstream gem5 uses CacheEntry (from base/cache/cache_entry.hh) instead
     * of the fork-local TaggedEntry.  CacheEntry stores the tag via a
     * TagExtractor functor rather than accepting a pre-extracted tag directly,
     * and does not carry an is_secure bit itself.  We therefore:
     *   - Pass the indexing policy's extractTag as the functor at construction.
     *   - Store the secure flag explicitly in the `secure` member.
     *
     * Note: upstream AssociativeCache::findEntry() / insertEntry() use a
     * single KeyType (Addr) and do not accept a separate is_secure argument.
     * The `secure` field is checked manually in updateHistoryTable() and
     * notifyFill() after the initial tag-based lookup, which is sufficient
     * because the PC-hashed key space makes cross-security collisions
     * negligibly rare in practice.
     */
    class HistoryTableEntry : public TableOfDeltasEntry, public CacheEntry
    {
      public:
        bool hysteresis = false;

        /**
         * Explicit secure flag.  CacheEntry does not carry one, so we
         * maintain it ourselves and propagate it on every insertEntry call.
         */
        bool secure = false;

        Addr pc = ~(0UL);
        /** FIFO of demand miss history. */
        std::list<HistoryInfo> history;

        /**
         * Default constructor — uses an identity tag extractor and zero-size
         * delta table.  Required so that AssociativeCache can value-initialise
         * entries before the real init_val is copy-assigned.
         */
        HistoryTableEntry()
            : TableOfDeltasEntry(0),
              CacheEntry([](Addr a) { return a; })
        {}

        /**
         * Normal constructor.
         * @param deltaTableSize  number of delta slots
         * @param ext             tag extractor from the indexing policy
         */
        HistoryTableEntry(int deltaTableSize, CacheEntry::TagExtractor ext)
            : TableOfDeltasEntry(deltaTableSize),
              CacheEntry(std::move(ext))
        {}
    };

    /**
     * History table backed by the upstream AssociativeCache.
     *
     * AssociativeCache<Entry> requires Entry to derive from ReplaceableEntry
     * (satisfied transitively via CacheEntry → ReplaceableEntry) and that
     * Entry::IndexingPolicy and Entry::KeyType are defined (both inherited
     * from CacheEntry as BaseIndexingPolicy and Addr respectively).
     *
     * Constructor argument order differs from AssociativeSet:
     *   AssociativeSet  : (assoc, num_entries, idx_policy, rpl_policy, init)
     *   AssociativeCache: (name,  num_entries, assoc, rpl_policy, idx_policy, init)
     */
    AssociativeCache<HistoryTableEntry> historyTable;


    Cycles hitSearchLatency;
    const bool aggressivePF;
    const bool useByteAddr;
    const bool triggerPht;

    struct BertiStats : public statistics::Group
    {
        BertiStats(statistics::Group *parent);

        statistics::Scalar trainOnHit;
        statistics::Scalar trainOnMiss;
        statistics::Scalar notifySkippedCond1;
        statistics::Scalar notifySkippedIsPF;
        statistics::Scalar notifySkippedNoEntry;
        statistics::Scalar entryEvicts;
    } statsBerti;


    /** Update history table on demand miss. */
    HistoryTableEntry* updateHistoryTable(const PrefetchInfo &pfi);

    /** Search for timely deltas. */
    void searchTimelyDeltas(HistoryTableEntry &entry,
                            const Cycles &latency,
                            const Cycles &demand_cycle,
                            const Addr &blk_addr);


    void printDeltaTableEntry(const TableOfDeltasEntry &entry) {
        DPRINTF(HWPrefetch, "Entry Counter: %d\n", entry.counter);
        for (auto &info : entry.deltas) {
            DPRINTF(HWPrefetch,
                    "=>[delta: %d coverage: %d status: %d]\n",
                    info.delta, info.coverageCounter, info.status);
        }
    }

    Addr pcHash(Addr pc) { return (pc >> 1); }

    int lastUsedBestDelta;
    int evictedBestDelta;

    boost::compute::detail::lru_cache<Addr, Addr> trainBlockFilter;

    std::unordered_map<int64_t, uint64_t> topDeltas;

    std::unordered_map<int64_t, uint64_t> evictedDeltas;

    const bool dumpTopDeltas;

    /**
     * Internal prefetch generation logic.
     *
     * Separated from the public calculatePrefetch override so that callers
     * (e.g. hierarchical prefetchers built on top of Berti) can retrieve the
     * best-delta target address via local_delta_pf_addr without needing to go
     * through the upstream Queued interface.
     */
    void calculatePrefetchImpl(const PrefetchInfo &pfi,
                               std::vector<AddrPriority> &addresses,
                               Addr &local_delta_pf_addr);

  public:

    /**
     * Shared prefetch filter.  Defaults to &trainBlockFilter; an outer
     * hierarchical prefetcher may point this at a wider shared filter.
     */
    boost::compute::detail::lru_cache<Addr, Addr> *filter;

    BertiPrefetcher(const BertiPrefetcherParams &p);

    /**
     * Upstream pure-virtual implementation.
     *
     * Queued::notify calls this signature.  We delegate to
     * calculatePrefetchImpl, discarding the local_delta_pf_addr output that
     * is only needed by fork-specific hierarchical prefetchers.
     */
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    int getEvictBestDelta() { return evictedBestDelta; }

    int getBestDelta() { return lastUsedBestDelta; }

    /**
     * Send a prefetch through the shared filter.
     *
     * Returns true if the address was not already cached in the filter and
     * was appended to `addresses`.
     *
     * @param using_best_delta_and_confident  when true, records the stride in
     *        lastUsedBestDelta so callers can query the dominant delta.
     */
    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                          std::vector<AddrPriority> &addresses, int prio,
                          bool using_best_delta_and_confident);

    /**
     * Upstream notifyFill override.
     *
     * Called on every demand cache fill; used to compute the actual miss
     * latency and search for timely deltas in the history table.
     *
     * The upstream Base::notifyFill takes a CacheAccessProbeArg (not a raw
     * PacketPtr), so the implementation extracts pkt = acc.pkt internally.
     */
    void notifyFill(const CacheAccessProbeArg &acc) override;

    bool shouldTrain(bool is_miss, const PrefetchInfo &pfi) {
        if (is_miss) {
            return true;
        } else {
            // Only train on hits if an entry for this PC already exists in the
            // same security domain.  The secure bit is not part of
            // AssociativeCache's KeyType so we check it explicitly after the
            // tag-based lookup.
            auto *e = historyTable.findEntry(pcHash(pfi.getPC()));
            return e != nullptr && e->secure == pfi.isSecure();
        }
    }

};

}

}


#endif
