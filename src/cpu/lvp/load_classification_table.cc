#include "cpu/lvp/load_classification_table.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/LCT.hh"
#include <math.h>

namespace gem5
{

LoadClassificationTable::LoadClassificationTable(const LoadClassificationTableParams &params)
    : SimObject(params),
      localPredictorSize(params.localPredictorSize),
      localCtrBits(params.localCtrBits),
      localPredictorSets(localPredictorSize / localCtrBits),
      localCtrs(localPredictorSets, SatCounter64(localCtrBits, 0)),
      localCtrThreads(localPredictorSets, 0),
      indexMask(localPredictorSets - 1),
      instShiftAmt(0),
      invalidateConstToZero(params.invalidateConstToZero)
{
    if (!isPowerOf2(localPredictorSize)) {
        fatal("Invalid LCT local predictor size!\n");
    }

    if (!isPowerOf2(localPredictorSets)) {
        fatal("Invalid number of LCT local predictor sets! Check localCtrBits.\n");
    }

    DPRINTF(LCT, "LCT index mask: %#x\n", indexMask);

    DPRINTF(LCT, "LCT size: %i\n",
            localPredictorSize);

    DPRINTF(LCT, "LCT counter bits: %i\n", localCtrBits);

    DPRINTF(LCT, "instruction shift amount: %i\n",
            instShiftAmt);
}

LVPType
LoadClassificationTable::lookup(ThreadID tid, Addr inst_addr)
{
    unsigned local_predictor_idx = getLocalIndex(inst_addr);

    if (tid != localCtrThreads[local_predictor_idx])
    {
        return LVP_STRONG_UNPREDICTABLE;
    }

    DPRINTF(LCT, "Thread ID: %d", tid);

    uint8_t counter_val = localCtrs[local_predictor_idx];



    LVPType classification = getPrediction(counter_val);

    DPRINTF(LCT, "During lookup index %#x had value %i\n",
            local_predictor_idx, classification);

    return classification;
}

LVPType
LoadClassificationTable::update(ThreadID tid, Addr inst_addr, LVPType prediction, bool prediction_correct)
{
    unsigned local_predictor_idx;

    // Update the local predictor.
    local_predictor_idx = getLocalIndex(inst_addr);

    if (tid == localCtrThreads[local_predictor_idx])
    {
        if (prediction_correct && !isConstant(local_predictor_idx)) {
            localCtrs[local_predictor_idx]++;

            // if (isConstant(local_predictor_idx)) {
            //     localCtrs[local_predictor_idx]--;
            // }
            DPRINTF(LCT, "Load classification updated with value %d after correct prediction.\n", localCtrs[local_predictor_idx]);
        } else {
            DPRINTF(LCT, "Load classification updated as incorrect.\n");
            if (prediction == LVP_CONSTANT && invalidateConstToZero) {
                resetCtr(local_predictor_idx);
            } else {
                localCtrs[local_predictor_idx]--;
            }
        }
    }
    else
    {
        // Destructive interference with a different thread, reset this index and update the thread
        localCtrThreads[local_predictor_idx] = tid;
        resetCtr(local_predictor_idx);
    }

    uint8_t counter_val = localCtrs[local_predictor_idx];
    return getPrediction(counter_val);
}

LVPType
LoadClassificationTable::strideUpdate(ThreadID tid, Addr inst_addr, LVPType prediction, bool prediction_correct, RegVal stride){
    unsigned local_predictor_idx;
    DPRINTF(LCT, "Stride update for thread %d at address %#x\n", tid, inst_addr);
    // Update the local predictor.
    local_predictor_idx = getLocalIndex(inst_addr);

    if (tid == localCtrThreads[local_predictor_idx])
    {
        if (prediction_correct) {
            DPRINTF(LCT, "Load classification updated as correct.\n");

            localCtrs[local_predictor_idx]++;
            // don't want to update to a constant if the stride isn't 0
            if (isConstant(local_predictor_idx) && stride != 0) {
                localCtrs[local_predictor_idx]--;
            }
        } else {
            DPRINTF(LCT, "Load classification updated as incorrect.\n");
            if (prediction == LVP_CONSTANT && invalidateConstToZero) {
                resetCtr(local_predictor_idx);
            } else {
                localCtrs[local_predictor_idx]--;
            }
        }
    }
    else
    {
        // Destructive interference with a different thread, reset this index and update the thread
        localCtrThreads[local_predictor_idx] = tid;
        resetCtr(local_predictor_idx);
    }

    uint8_t counter_val = localCtrs[local_predictor_idx];
    return getPrediction(counter_val);
}

// CHECK THIS METHOD...... I SUSPECT THE hasContext PART MIGHT BE WONKY
LVPType
LoadClassificationTable::contextUpdate(ThreadID tid, Addr inst_addr, LVPType prediction, bool prediction_correct, bool hasContext)
{
    unsigned local_predictor_idx;

    // Update the local predictor.
    local_predictor_idx = getLocalIndex(inst_addr);

    if (tid == localCtrThreads[local_predictor_idx])
    {
        if (prediction_correct) {
            DPRINTF(LCT, "Load classification updated as correct.\n");
            // Don't update to a constant if it has a context (i.e. the value is being predicted)
            if (hasContext) {
                localCtrs[local_predictor_idx]++;
            }

        } else {
            DPRINTF(LCT, "Load classification updated as incorrect.\n");
            if (prediction == LVP_CONSTANT && invalidateConstToZero) {
                resetCtr(local_predictor_idx);
            } else {
                localCtrs[local_predictor_idx]--;
            }
        }
    }
    else
    {
        // Destructive interference with a different thread, reset this index and update the thread
        localCtrThreads[local_predictor_idx] = tid;
        resetCtr(local_predictor_idx);
    }

    uint8_t counter_val = localCtrs[local_predictor_idx];
    return getPrediction(counter_val);
}

void
LoadClassificationTable::reset()
{
    auto zeroCounter = new SatCounter64(localCtrBits, 0);
    for (int c = 0; c < localPredictorSets; c++)
    {
        localCtrs[c] = *zeroCounter;
    }

    delete zeroCounter;
}

void
LoadClassificationTable::resetCtr(unsigned local_predictor_idx)
{
    while (localCtrs[local_predictor_idx] != LVP_STRONG_UNPREDICTABLE) {
        localCtrs[local_predictor_idx]--;
    }
}


LVPType
LoadClassificationTable::getPrediction(uint8_t &count)
{
    // If MSB is 0, value is unpredictable
    // If counter is saturated, value is constant
    // Otherwise, the value is predictable
    DPRINTF(LCT, "Counter value: %i, localCtrBits: %d\n", count, localCtrBits);
    return (count == 0) ? LVP_STRONG_UNPREDICTABLE
            : (count >> (localCtrBits - 1)) == 0 ? LVP_WEAK_UNPREDICTABLE
            // : count == ((1 << localCtrBits) -1) ? LVP_CONSTANT
            : LVP_PREDICTABLE;
}


bool
LoadClassificationTable::isConstant(int local_predictor_idx)
{
    // if (localCtrBits == 4){
    //     return localCtrs[local_predictor_idx] == 7;
    // }
    return localCtrs[local_predictor_idx] == ((1 << localCtrBits) - 1);
}

unsigned
LoadClassificationTable::getLocalIndex(Addr &inst_addr)
{
    return (inst_addr >> instShiftAmt) & indexMask;
}

} // namespace gem5
