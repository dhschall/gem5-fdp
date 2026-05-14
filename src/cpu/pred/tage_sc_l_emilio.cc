#include "cpu/pred/tage_sc_l_emilio.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/Fetch.hh"
#include "debug/Tage.hh"

namespace gem5
{

namespace branch_prediction
{

// Changed: call ConditionalPredictor(params) instead of BPredUnit(params).
TAGE_EMILIO::TAGE_EMILIO(const TAGE_EMILIOParams &params)
    : ConditionalPredictor(params), tage(1024)
{
}

// PREDICTOR UPDATE
void
TAGE_EMILIO::update(ThreadID tid, Addr pc, bool taken, void * &bp_history,
              bool squashed, const StaticInstPtr & inst, Addr target)
{
    TageEmilioBranchInfo *bi = static_cast<TageEmilioBranchInfo*>(bp_history);

    DPRINTF(Tage, "TAGE id:%d update: %lx squashed:%s bp_history:%p\n", bi ? bi->id : -1, pc, squashed, bp_history);

    assert(bp_history);
    if (squashed) {
        // This restores the global history, then update it
        // and recomputes the folded histories.
        tage.flush_branch_and_repair_state(bi->id, pc, bi->br_type, taken, target);
        return;
    }

    tage.commit_state(bi->id, pc, bi->br_type, taken);
    tage.commit_state_at_retire(bi->id, pc, bi->br_type, taken, target);
    delete bi;
    bp_history = nullptr;
}

void
TAGE_EMILIO::squash(ThreadID tid, void * &bp_history)
{
    TageEmilioBranchInfo *bi = static_cast<TageEmilioBranchInfo*>(bp_history);
    DPRINTF(Tage, "TAGE id: %d squash: %lx bp_history:%p\n", bi ? bi->id : -1,
        bi? bi->pc : 0x00, bp_history);
    if (bi) {
      tage.flush_branch(bi->id);
    }
    delete bi;
    bp_history = nullptr;
}

bool
TAGE_EMILIO::predict(ThreadID tid, Addr pc, bool cond_branch, void* &b)
{
    uint32_t id = tage.get_new_branch_id();
    TageEmilioBranchInfo *bi = new TageEmilioBranchInfo();
    b = (void*)(bi);
    DPRINTF(Tage, "TAGE id: %d predict: %lx bp_history:%p\n", id, pc, b);
    bi->id = id;
    bi->pc = pc;
    bi->br_type.is_conditional = cond_branch;
    bi->br_type.is_indirect = false;
    return tage.get_prediction(id, pc);
}

bool
TAGE_EMILIO::lookup(ThreadID tid, Addr pc, void* &bp_history)
{
    DPRINTF(Tage, "TAGE lookup: %lx %p\n", pc, bp_history);
    bool retval = predict(tid, pc, true, bp_history);

    DPRINTF(Tage, "Lookup branch: %lx; predict:%d; bp_history:%p\n", pc, retval, bp_history);

    return retval;
}

// Changed: the ConditionalPredictor interface now passes the StaticInstPtr
// so that direction predictors can inspect the instruction if desired.
// TAGE-SC-L does not need it, so inst is accepted but unused.
void
TAGE_EMILIO::updateHistories(ThreadID tid, Addr pc, bool uncond,
                         bool taken, Addr target,
                         const StaticInstPtr &inst, void * &bp_history)
{
    TageEmilioBranchInfo *bi = static_cast<TageEmilioBranchInfo*>(bp_history);

    DPRINTF(Tage, "TAGE id: %d updateHistories: %lx %p\n", bi ? bi->id : -1, pc, bp_history);

    assert(uncond || bp_history);
    if (uncond) {
        DPRINTF(Tage, "UnConditionalBranch: %lx\n", pc);
        predict(tid, pc, false, bp_history);
    }

    bi = static_cast<TageEmilioBranchInfo*>(bp_history);
    // Update the global history for all branches
    tage.update_speculative_state(bi->id, pc, bi->br_type, taken, target);
}

} // namespace branch_prediction
} // namespace gem5
