/*
 * Copyright (c) 2024 Pranith Kumar
 * All rights reserved.
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

/**
 * @file
 * Declaration of a BTB entry and BTB indexing policy.
 */

#ifndef __CPU_PRED_BTB_ENTRY_HH__
#define __CPU_PRED_BTB_ENTRY_HH__

#include <vector>

#include "arch/generic/pcstate.hh"
#include "base/intmath.hh"
#include "base/types.hh"
#include "base/sat_counter.hh"
#include "cpu/static_inst.hh"
#include "cpu/pred/branch_type.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/base.hh"
#include "params/BTBIndexingPolicy.hh"
#include "params/BTBSetAssociative.hh"

namespace gem5 {

class BTBTagType
{
  public:
    struct KeyType
    {
        Addr address;
        ThreadID tid;
    };
    using Params = BTBIndexingPolicyParams;
};

using BTBIndexingPolicy = IndexingPolicyTemplate<BTBTagType>;
template class IndexingPolicyTemplate<BTBTagType>;

class BTBSetAssociative : public BTBIndexingPolicy
{
  public:
    PARAMS(BTBSetAssociative);
    using KeyType = BTBTagType::KeyType;

    BTBSetAssociative(const Params &p)
        : BTBIndexingPolicy(p, p.num_entries, p.set_shift),
          tagMask(mask(p.tag_bits))
    {
        setNumThreads(p.numThreads);
    }

  protected:
    /**
     * Extract the set index for the instruction PC based on tid.
     */
    uint32_t
    extractSet(const KeyType &key) const
    {
        return ((key.address >> setShift)
                ^ (key.tid << (tagShift - setShift - log2NumThreads)))
            & setMask;
    }

  public:
    /**
     * Find all possible entries for insertion and replacement of an address.
     */
    std::vector<ReplaceableEntry*>
    getPossibleEntries(const KeyType &key) const override
    {
        auto set_idx = extractSet(key);

        assert(set_idx < sets.size());

        return sets[set_idx];
    }

    /**
     * Set number of threads sharing the BTB
     */
    void
    setNumThreads(unsigned num_threads)
    {
        log2NumThreads = log2i(num_threads);
    }

    /**
     * Generate the tag from the given address.
     */
    Addr
    extractTag(const Addr addr) const override
    {
        return (addr >> tagShift) & tagMask;
    }

    Addr regenerateAddr(const KeyType &key,
                        const ReplaceableEntry* entry) const override
    {
        panic("Not implemented!");
        return 0;
    }

  private:
    const uint64_t tagMask;
    unsigned log2NumThreads;
};

namespace branch_prediction
{

class BTBEntry : public ReplaceableEntry
{
  public:
    using IndexingPolicy = gem5::BTBIndexingPolicy;
    using KeyType = gem5::BTBTagType::KeyType;
    using TagExtractor = std::function<Addr(Addr)>;

    /** Default constructor */
    BTBEntry(TagExtractor ext)
        : inst(nullptr), extractTag(ext), valid(false), tag({MaxAddr, -1}), branchAddr(0),
        prefetched(false), takenPrefetched(false), triggeredByPBHit(false),
        prefetchedFromL3(false), timestamp(0),
        prefetchDistance(0), predTaken(false), fromPBuffer(false), markovPredType(-1),
        prefetchThrough(false), prefetchTarget(false), toL1(false),
        prefetchTriggerType(BranchType::NoBranch),
        dir(2, 1) // Two bits, weakly not taken
    {}

    /** Update the target and instruction in the BTB entry.
     *  During insertion, only the tag (key) is updated.
     */
    void
    update(const PCStateBase &_target,
           StaticInstPtr _inst)
    {
        set(target, _target);
        inst = _inst;
    }

    void
    update(const BTBEntry &other)
    {
        inst       = other.inst;
        branchAddr = other.branchAddr;
        set(target, other.target);
        prefetchThrough |= other.prefetchThrough;
        prefetchTarget = other.prefetchTarget;
        dir = other.dir;
    }

    /**
     * Checks if the given tag information corresponds to this entry's.
     */
    bool
    match(const KeyType &key) const
    {
        return isValid() && (tag.address == extractTag(key.address))
            && (tag.tid == key.tid);
    }

    /**
     * Insert the block by assigning it a tag and marking it valid. Touches
     * block if it hadn't been touched previously.
     */
    void
    insert(const KeyType &key)
    {
        setValid();
        setTag({extractTag(key.address), key.tid});
        branchAddr = key.address;

        // Reset all states since this is a new block or recycled victim
        prefetched = false;
        takenPrefetched = false;
        triggeredByPBHit = false;
        prefetchedFromL3 = false;
        timestamp = Cycles(0);
        prefetchDistance = 0;
        predTaken = false;
        fromPBuffer = false;
        markovPredType = -1;
        prefetchThrough = false;
        prefetchTarget = false;
        toL1 = false;
        prefetchTriggerType = BranchType::NoBranch;
        dir.reset();
    }

    /** Copy constructor */
    BTBEntry(const BTBEntry &other)
        : dir(other.dir)
    {
        valid      = other.valid;
        tag        = other.tag;
        inst       = other.inst;
        extractTag = other.extractTag;
        branchAddr = other.branchAddr;
        set(target, other.target);
        prefetched = other.prefetched;
        takenPrefetched = other.takenPrefetched;
        triggeredByPBHit = other.triggeredByPBHit;
        timestamp = other.timestamp;
        prefetchDistance = other.prefetchDistance;
        predTaken = other.predTaken;
        fromPBuffer = other.fromPBuffer;
        markovPredType = other.markovPredType;
        prefetchThrough = other.prefetchThrough;
        prefetchTarget = other.prefetchTarget;
        toL1 = other.toL1;
        prefetchTriggerType = other.prefetchTriggerType;
    }

    /** Assignment operator */
    BTBEntry& operator=(const BTBEntry &other)
    {
        valid      = other.valid;
        tag        = other.tag;
        inst       = other.inst;
        extractTag = other.extractTag;
        branchAddr = other.branchAddr;
        set(target, other.target);
        prefetched = other.prefetched;
        takenPrefetched = other.takenPrefetched;
        triggeredByPBHit = other.triggeredByPBHit;
        timestamp = other.timestamp;
        prefetchDistance = other.prefetchDistance;
        predTaken = other.predTaken;
        fromPBuffer = other.fromPBuffer;
        markovPredType = other.markovPredType;
        prefetchThrough = other.prefetchThrough;
        prefetchTarget = other.prefetchTarget;
        toL1 = other.toL1;
        prefetchTriggerType = other.prefetchTriggerType;
        dir = other.dir;

        return *this;
    }

    void copyState(const BTBEntry &other) {
        prefetched = other.prefetched;
        takenPrefetched = other.takenPrefetched;
        triggeredByPBHit = other.triggeredByPBHit;
        timestamp = other.timestamp;
        prefetchDistance = other.prefetchDistance;
        predTaken = other.predTaken;
        fromPBuffer = other.fromPBuffer;
        markovPredType = other.markovPredType;
        prefetchThrough = other.prefetchThrough;
        prefetchTarget = other.prefetchTarget;
        toL1 = other.toL1;
        prefetchTriggerType = other.prefetchTriggerType;
        dir = other.dir;
    }

    /**
     * Checks if the entry is valid.
     */
    bool isValid() const { return valid; }

    /**
     * Get tag associated to this block.
     */
    KeyType getTag() const { return tag; }

    /** Invalidate the block. Its contents are no longer valid. */
    void
    invalidate()
    {
        valid = false;
        setTag({MaxAddr, -1});
    }

    /** The entry's target. */
    std::unique_ptr<PCStateBase> target;

    /** Pointer to the static branch inst at this address */
    StaticInstPtr inst;

    std::string
    print() const override
    {
        return csprintf("tag: %#x tid: %d valid: %d | %s", tag.address, tag.tid,
                        isValid(), ReplaceableEntry::print());
    }

  protected:
    /**
     * Set tag associated to this block.
     */
    void setTag(KeyType _tag) { tag = _tag; }

    /** Set valid bit. The block must be invalid beforehand. */
    void
    setValid()
    {
        assert(!isValid());
        valid = true;
    }

  private:
    /** Callback used to extract the tag from the entry */
    TagExtractor extractTag;

    /**
     * Valid bit. The contents of this entry are only valid if this bit is set.
     * @sa invalidate()
     * @sa insert()
     */
    bool valid;

    /** The entry's tag. */
    KeyType tag;

    /** PC of this entry */
    Addr branchAddr;

    /** Whether this L1 entry was prefetched and hasn't been hit yet. */
    bool prefetched;

    /** For counting statistics */
    bool takenPrefetched;

    bool triggeredByPBHit;

    /** True when this pBuffer entry was filled from L3 (not L2). */
    bool prefetchedFromL3;

  public:
    Addr getBranchAddr() const { return branchAddr; }
    bool isPrefetched() const { return prefetched; }
    void setPrefetched(bool p) { prefetched = p; }

    bool isTakenPrefetched() const { return takenPrefetched; }
    void setTakenPrefetched(bool p) { takenPrefetched = p; }

    bool isTriggeredByPBHit() const { return triggeredByPBHit; }
    void setTriggeredByPBHit(bool p) { triggeredByPBHit = p; }

    bool isPrefetchedFromL3() const { return prefetchedFromL3; }
    void setPrefetchedFromL3(bool p) { prefetchedFromL3 = p; }


    Cycles getTimestamp() const { return timestamp; }
    void setTimestamp(Cycles t) { timestamp = t; }

    uint8_t getPrefetchDistance() const { return prefetchDistance; }
    void setPrefetchDistance(uint8_t d) { prefetchDistance = d; }

  private:
    /** Timestamp when the entry was prefetched. */
    Cycles timestamp;

    /** The distance (index) of this branch within the prefetched block. */
    uint8_t prefetchDistance;

    /** Prediction result stored during prefetch. */
    bool predTaken;

  public:
    bool getPredTaken() const { return predTaken; }
    void setPredTaken(bool p) { predTaken = p; }

    /** Whether this L1 entry was installed from pBuffer (for L1 reuse tracking). */
    bool isFromPBuffer() const { return fromPBuffer; }
    void setFromPBuffer(bool p) { fromPBuffer = p; }

    /** Markov predecessor branch type for shadow prefetch tracking (Policy 11).
     *  -1 = no predecessor (demand fill from L2, not prefetched). */
    int8_t getMarkovPredType() const { return markovPredType; }
    void setMarkovPredType(int8_t t) { markovPredType = t; }

    /** Policy 12: Whether the fall-through BTB entry should be prefetched. */
    bool getPrefetchThrough() const { return prefetchThrough; }
    void setPrefetchThrough(bool p) { prefetchThrough = p; }

    /** Policy 12: Whether the taken-target BTB entry should be prefetched. */
    bool getPrefetchTarget() const { return prefetchTarget; }
    void setPrefetchTarget(bool p) { prefetchTarget = p; }

  private:
    /** Flag to track if this entry was installed from prefetch buffer */
    bool fromPBuffer;

    /** Predecessor branch type that triggered this Markov prefetch */
    int8_t markovPredType;

    /** Policy 12: prefetch training bits for block-based BTB */
    bool prefetchThrough;
    bool prefetchTarget;

    /** Whether this prefetch-queued entry targets L1 (true) or pBuffer (false). */
    bool toL1;

    BranchType prefetchTriggerType;
  public:
    bool getToL1() const { return toL1; }
    void setToL1(bool v) { toL1 = v; }

    BranchType getPrefetchTriggerType() const
    {
        return prefetchTriggerType;
    }
    void setPrefetchTriggerType(BranchType t)
    {
        prefetchTriggerType = t;
    }

  private:
    SatCounter8 dir;
  public:
    bool getDir() const {
        return dir > 1; // 0,1 not taken, 1,2 taken
    }
    void updateDir(bool taken) {
        dir += taken ? 1 : -1;
    }
    void copyDir(const BTBEntry& other) {
        dir = other.dir;
    }

};
} // namespace gem5::branch_prediction
/**
 * This helper generates a tag extractor function object
 * which will be typically used by Replaceable entries indexed
 * with the BaseIndexingPolicy.
 * It allows to "decouple" indexing from tagging. Those entries
 * would call the functor without directly holding a pointer
 * to the indexing policy which should reside in the cache.
 */
static constexpr auto
genTagExtractor(BTBIndexingPolicy *ip)
{
    return [ip] (Addr addr) { return ip->extractTag(addr); };
}

}

#endif //__CPU_PRED_BTB_ENTRY_HH__
