#ifndef __CPU_PRED_MULTILEVEL_BTB_HH__
#define __CPU_PRED_MULTILEVEL_BTB_HH__

#include "base/cache/associative_cache.hh"
#include "cpu/pred/btb.hh"
#include "cpu/pred/btb_entry.hh"
#include "params/MultiLevelBTB.hh"

namespace gem5::branch_prediction
{

// Simplified L1 BTB entry. Only store the branch address.
struct L1BTBEntry : public ReplaceableEntry
{
    using IndexingPolicy = gem5::BTBIndexingPolicy;
    using KeyType = gem5::BTBTagType::KeyType;
    
    Addr instPC;
    ThreadID tid;
    bool valid;
    
    L1BTBEntry() : instPC(0), tid(0), valid(false) {}
    
    void update(Addr pc, ThreadID thread_id) {
        instPC = pc;
        tid = thread_id;
        valid = true;
    }
    
    
    // Required methods for AssociativeCache
    bool match(const KeyType &key) const {
        return valid && (instPC == key.address) && (tid == key.tid);
    }
    
    bool isValid() const { return valid; }
    
    void insert(const KeyType &key) {
        instPC = key.address;
        tid = key.tid;
        valid = true;
    }
    
    void invalidate() {
        valid = false;
        instPC = 0;
        tid = 0;
    }
};

class MultiLevelBTB : public BranchTargetBuffer
{
  public:
    MultiLevelBTB(const MultiLevelBTBParams &params);

    void memInvalidate() override;
    bool valid(ThreadID tid, Addr instPC) override;
    
    const PCStateBase *lookup(ThreadID tid, Addr instPC,
                              BranchType type = BranchType::NoBranch) override;
    
    BTBLookupResult lookupWithLatency(ThreadID tid, Addr instPC,
                                      BranchType type = BranchType::NoBranch) override;
    
    void update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                BranchType type = BranchType::NoBranch,
                StaticInstPtr inst = nullptr) override;
    
    const StaticInstPtr getInst(ThreadID tid, Addr instPC) override;
    
    Cycles getStaticLatency() const override { return staticLatency; }

  private:
    L1BTBEntry *findL1Entry(Addr instPC, ThreadID tid);
    
    BTBEntry *findL2Entry(Addr instPC, ThreadID tid);

    AssociativeCache<L1BTBEntry> l1btb;
    
    AssociativeCache<BTBEntry> l2btb;
    
    const Cycles l1Latency;
    const Cycles l2Latency;
    
    const unsigned l1NumEntries;
};

} // namespace gem5::branch_prediction

#endif // __CPU_PRED_MULTILEVEL_BTB_HH__