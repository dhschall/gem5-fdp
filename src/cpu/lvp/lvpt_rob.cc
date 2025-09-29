#include "cpu/lvp/load_value_prediction_table.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/LVPT.hh"

LoadValuePredictionTable::LoadValuePredictionTable(const LoadValuePredictionTableParams *params)
    : SimObject(params),
      numEntries(params->entries),
      historyDepth(params->historyDepth),
      indexMask(numEntries - 1),
      instShiftAmt(0) // TODO what is correct???
{
    if (!isPowerOf2(numEntries)) {
        fatal("Invalid LVPT number of entries!\n");
    }

    DPRINTF(LVPT, "LVPT index mask: %#x\n", indexMask);

    DPRINTF(LVPT, "LVPT entries: %i\n",
            numEntries);

    DPRINTF(LVPT, "LVPT history depth: %i\n", historyDepth);

    DPRINTF(LVPT, "instruction shift amount: %i\n",
            instShiftAmt);
}

uint64_t
LoadValuePredictionTable::lookup(ThreadID tid, Addr inst_addr)
{
    unsigned local_predictor_idx = getLocalIndex(inst_addr);

    DPRINTF(LVPT, "Looking up index %#x\n",
            local_predictor_idx);

    // TODO lookup value in table
    return 0;
}

void
LoadValuePredictionTable::update(ThreadID tid, Addr inst_addr, bool prediction_correct,
                bool squashed, const StaticInstPtr & inst, Addr corrTarget)
{
    unsigned local_predictor_idx;

    // No state to restore, and we do not update on the wrong
    // path.
    if (squashed) {
        return;
    }

    // Update the local predictor.
    local_predictor_idx = getLocalIndex(inst_addr);

    DPRINTF(LVPT, "Looking up index %#x\n", local_predictor_idx);
}

inline
unsigned
LoadValuePredictionTable::getLocalIndex(Addr &inst_addr)
{
    return (inst_addr >> instShiftAmt) & indexMask;
}

LoadValuePredictionTable*
LoadValuePredictionTableParams::create()
{
    return new LoadValuePredictionTable(this);
}
