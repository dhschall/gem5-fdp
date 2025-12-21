#ifndef __CPU_PRED_BR_RECYCLING_DYN_INST_HH__
#define __CPU_PRED_BR_RECYCLING_DYN_INST_HH__

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/pred/conditional.hh"
#include "cpu/pred/tage_sc_l.hh"
#include "params/BranchRecyclingCacheDynInst.hh"
#include "sim/probe/probe.hh"

namespace gem5
{
namespace branch_prediction
{

class BranchRecyclingCacheDynInst : public ConditionalPredictor
{
  public:
    using Params = BranchRecyclingCacheDynInstParams;
    BranchRecyclingCacheDynInst(const Params &p);

    bool lookup(ThreadID tid, Addr pc, void *&bp_history) override;

    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                           void *&bpHistory) override;

    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void *&bp_history) override;

    void update(ThreadID tid, Addr pc, bool taken, void *&bp_history,
                bool squashed, const StaticInstPtr &inst,
                Addr target) override;

    void squash(ThreadID tid, void *&bp_history) override;

    void regProbeListeners() override;

    void dump(const std::string& filename);

    void
    setCPU(BaseCPU *_cpu)
    {
        cpu = _cpu;
    }

  private:
    // Branch outcome
    struct Entry
    {
      //Attributes for recycling
        Addr pc = 0;
        bool hasOutcome = false;
        bool taken = false;
        BranchType brType = BranchType::NoBranch;
        InstSeqNum seqNum = 0;
        bool valid = true;


    };

    struct recyclingEntry
    {
        //Attribtes for training
        int trainCounter = 0;

        int striteCounterValue = 0;
        int striteCounterPC = 0;

        //Attributes for StriteDynInst
        Addr lastDynAddr = 0;
        int lastDynAddrOffset = 0;
        int striteCounterDynAddr = 0;

        //Attributes for Statistics
        int correctPredictionRecycling = 0;
        int incorrectPredictionRecycling = 0;

        int correctPredictionBase = 0;
        int incorrectPredictionBase = 0;

        int bothWrong = 0;
        int bothCorrect = 0;

        //Stack of entries for recycling
        std::vector<Entry> entries;
    };

    // Per-lookup history
    struct History
    {
        Addr pc = 0;

        bool usedRecycle = false;
        bool recycledTaken = false;

        bool actualKnown = false;
        bool actualTaken = false;

        BranchType brType = BranchType::DirectCond;

        unsigned brpIdx = 0;

        bool base_pred = false;

        void *tage_bi = nullptr;
    };

    //Attributes for StritePC
    Addr lastPC = 0;
    int stritePCCounter = 0;

    //Attributes for StriteDynAddr

    InstSeqNum lastSeqNum = 0;

    // Base predictor
    TAGE_SC_L *base;
    const bool enableRecycling;
    const bool enableTraining;
    const bool enableStriteValue;
    const bool enableStritePC;

    // Stack with the Addr and a tuple for training aswell as entries
    //Track statistics here
    std::unordered_map<Addr,  recyclingEntry> dynInstStacks;

    // Map dynamic instance to its static PC
    std::unordered_map<InstSeqNum, Addr> seqToPc;

    // Probes
    ProbeListenerPtr<> listener;  // ToCommit
    ProbeListenerPtr<> slistener; // SquashInst
    ProbeListenerPtr<> blistener; // SquashInst
    BaseCPU *cpu;

    // Probe handlers
    void notifyExecutedInst(const o3::DynInstPtr &inst);
    void notifySquashedInst(const o3::DynInstPtr &mispred_inst,
                            const o3::DynInstPtr &sq_inst);
    void notifyDependentInst(const o3::DynInstPtr &branch_i,
                            const o3::DynInstPtr &load_i);

    // Helpers
    void pushExecutedOutcome(const o3::DynInstPtr &inst);

    struct branch_info
    {
      int exec = 0;
      int taken = 0;
      int mispred = 0;
    };
    std::unordered_map<Addr,branch_info> branchStats;

  public:
    struct BranchRecyclingCacheDynInstStats : public statistics::Group
    {
        BranchRecyclingCacheDynInstStats(statistics::Group *parent);
        statistics::Scalar recycledPred;
        statistics::Scalar basePred;
        statistics::Scalar RecycleBaseDiffer;
        statistics::Scalar missPredictedFaultyRecycle;
        statistics::Scalar missPredictedTageFault;
        statistics::Scalar missPredictedBothPredictorsWrong;
        statistics::Scalar missPredicts;
        statistics::Scalar committedCount;
    } stats;
};

} // namespace branch_prediction
} // namespace gem5

#endif
