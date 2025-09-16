#ifndef __CPU_LVP_LOADVALUEPREDICTIONTABLE_HH__
#define __CPU_LVP_LOADVALUEPREDICTIONTABLE_HH__

#include "base/types.hh"
#include "cpu/static_inst.hh"
#include "sim/sim_object.hh"

#include "enums.hh"
#include "params/LoadValuePredictionTable.hh"

/**
 * Implements storage of values for the load value locality prediction.
 */
class LoadValuePredictionTable : public SimObject
{
  public:
    /**
     * Build load classfication table.
     */
    LoadValuePredictionTable(const LoadValuePredictionTableParams *params);

    /**
     * Looks up the given instruction address in the LVPT and returns
     * a value for the instruction address.
     * @param inst_addr The address of the instruction to look up.
     * @param bp_history Pointer to any bp history state.
     * @return Whether or not the branch is taken.
     */
    uint64_t lookup(ThreadID tid, Addr inst_addr);

    /**
     * Updates the branch predictor with the actual result of a branch.
     * @param inst_addr The address of the instruction to update.
     * @param taken Whether or not the branch was taken.
     */
    void update(ThreadID tid, Addr inst_addr, bool prediction_correct,
                bool squashed, const StaticInstPtr & inst, Addr corrTarget);

  private:
    /** Calculates the local index based on the PC. */
    inline unsigned getLocalIndex(Addr &PC);

    /** Size of the local predictor. */
    const unsigned numEntries;

    /** Number of bits of the local predictor's counters. */
    const unsigned historyDepth;

    /** Mask to get index bits. */
    const unsigned indexMask;

    /** Number of bits to shift instructions by for predictor addresses. */
    const unsigned instShiftAmt;
};

#endif // __CPU_LVP_LOADVALUEPREDICTIONTABLE_HH__
