#ifndef __CPU_PRED_MULTILEVEL_BTB_HH__
#define __CPU_PRED_MULTILEVEL_BTB_HH__

#include "base/cache/associative_cache.hh"
#include "cpu/pred/btb.hh"
#include "cpu/pred/btb_entry.hh"
#include "params/MultiLevelBTB.hh"

namespace gem5::branch_prediction
{


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
   

  private:
    BTBEntry *findL1Entry(Addr instPC, ThreadID tid);
    
    BTBEntry *findL2Entry(Addr instPC, ThreadID tid);

    AssociativeCache<BTBEntry> l1btb;
    
    AssociativeCache<BTBEntry> l2btb;
    
    const Cycles l1Latency;
    const Cycles l2Latency;

    // Multi-level BTB specific statistics
    struct MultiLevelBTBStats : public statistics::Group
    {
        MultiLevelBTBStats(statistics::Group *parent);
        statistics::Vector l1Hits;
        statistics::Vector l2Hits;
        
    } multilevelstats;
};

} // namespace gem5::branch_prediction

#endif // __CPU_PRED_MULTILEVEL_BTB_HH__