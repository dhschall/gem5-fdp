#ifndef __CPU_PRED_TAGE_EMILIO_HH__
#define __CPU_PRED_TAGE_EMILIO_HH__

#include <vector>

#include "base/types.hh"
#include "cpu/pred/conditional.hh"  // Changed: was bpred_unit.hh
#include "cpu/pred/tage_base.hh"
#include "params/TAGE_EMILIO.hh"
#include "cpu/pred/tagescl/tagescl.hpp"

namespace gem5
{

namespace branch_prediction
{

// Changed: now extends ConditionalPredictor instead of BPredUnit.
// In the refactored gem5 branch prediction framework, BPredUnit is the
// top-level orchestrator that composes a ConditionalPredictor, an
// IndirectPredictor, a BTB, and a RAS.  Direction predictors (like
// TAGE-SC-L) live as ConditionalPredictor subclasses.
class TAGE_EMILIO: public ConditionalPredictor
{
  private:
    tagescl::Tage_SC_L<tagescl::CONFIG_128KB> tage;

  protected:
    virtual bool predict(ThreadID tid, Addr branch_pc, bool cond_branch,
                         void* &b);

    struct TageEmilioBranchInfo
    {
        uint32_t id;
        Addr pc;
        tagescl::Branch_Type br_type;
        TageEmilioBranchInfo()
        {}
    };

  public:

    TAGE_EMILIO(const TAGE_EMILIOParams &params);

    // ConditionalPredictor interface.
    bool lookup(ThreadID tid, Addr pc, void* &bp_history) override;

    // Changed: new ConditionalPredictor interface adds a StaticInstPtr
    // parameter so the predictor can inspect the instruction if needed.
    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void * &bp_history) override;

    void update(ThreadID tid, Addr pc, bool taken,
                void * &bp_history, bool squashed,
                const StaticInstPtr & inst, Addr target) override;

    void squash(ThreadID tid, void * &bp_history) override;

    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond, void * &bpHistory);

};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_TAGE_EMILIO_HH__
