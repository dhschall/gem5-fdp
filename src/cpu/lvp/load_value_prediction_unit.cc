#include "cpu/lvp/load_value_prediction_unit.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/LVP.hh"

namespace gem5
{

LoadValuePredictionUnit::LoadValuePredictionUnit(const LoadValuePredictionUnitParams &params) :
    SimObject(params),
    loadClassificationTable(params.load_classification_table),
    loadValuePredictionTable(params.load_value_prediction_table),
    constantVerificationUnit(params.constant_verification_unit),
    isStride(params.is_stride), isContext(params.is_context),
    numPredictableLoads(0), numPredictableCorrect(0), numPredictableIncorrect(0),
    numConstLoads(0), numConstLoadsMispredicted(0), numConstLoadsCorrect(0),
    totalLoads(0), numZeroConstLoads(0), numOneConstLoads(0)
{
    DPRINTF(LVP, "Created the LVP\n");
    panic_if(!loadClassificationTable, "LVP must have a non-null LCT");
    panic_if(!loadValuePredictionTable, "LVP must have a non-null LVPT");
    panic_if(!constantVerificationUnit, "LVP must have a non-null LVPT");

    if (isStride) {
        assert(!isContext);
        loadValuePredictionTable->strideInitialize();

    }
}

LvptResult
LoadValuePredictionUnit::lookup(ThreadID tid, Addr inst_addr)
{
    totalLoads++;
    bool lvptResultValid = false;
    RegVal lvptResult;
    if (isStride){
        lvptResult = loadValuePredictionTable->strideLookup(tid, inst_addr, &lvptResultValid);
    }
    else if (isContext) {
        lvptResult = loadValuePredictionTable->contextLookup(tid, inst_addr, &lvptResultValid);
    }
    else{
        lvptResult = loadValuePredictionTable->lookup(tid, inst_addr, &lvptResultValid);
    }
    
    auto lctResult = lvptResultValid ? loadClassificationTable->lookup(tid, inst_addr) : LVP_STRONG_UNPREDICTABLE;

    LvptResult result;
    result.taken = lctResult;
    result.value = lvptResult;

    // if(lctResult == LVP_CONSTANT)
    // {
    //     lctResult = LVP_PREDICTABLE;

    //     // DPRINTF(LVP, "Constant load for thread %d at address %#x had value %d\n",
    //     //     tid, inst_addr, lvptResult);
    //     // if(lvptResult == 0) numZeroConstLoads++;
    //     // else if(lvptResult == 1) numOneConstLoads++;
    // }

    // Stat collection
    if(lctResult == LVP_CONSTANT)
        numConstLoads++;
    else if(lctResult == LVP_PREDICTABLE)
        numPredictableLoads++;

    return result;
}

bool
LoadValuePredictionUnit::processStoreAddress(ThreadID tid, Addr store_address)
{
    DPRINTF(LVP, "Store address lookup for address: 0x%x\n", store_address);
    constantVerificationUnit->processStoreAddress(tid, store_address);
    return true;
}

bool
LoadValuePredictionUnit::verifyPrediction(ThreadID tid, Addr pc, Addr load_address,
                                    RegVal correct_val, RegVal predicted_val,
                                    LVPType classification) {
    /**
     * LVPT: lvpt::update(pc, tid, correct_val)
     * LCT:  lct::update(pc, tid) retval lctResult
     * CVU: if(lctResult = constant) updateCVU(pc, tid, pc[9:2], load_address);
     */
    DPRINTF(LVP, "[TID: %d] Verifying prediction for load instruction 0x%x. predicted val: %d, true value: %d, classification: %d, from addr: 0x%x\n", tid, pc, predicted_val, correct_val, classification, load_address);
    if(predicted_val != correct_val && classification == LVP_PREDICTABLE) {
        // Stats for predictable loads here
        numPredictableIncorrect++;
    }
    else if(predicted_val == correct_val && classification == LVP_PREDICTABLE) {
        numPredictableCorrect++;
    }
    if (isContext){
        loadValuePredictionTable->contextUpdate(pc, correct_val, tid);
    }
    else{
        loadValuePredictionTable->update(pc, correct_val, tid);
    }
    
    if(classification != LVP_CONSTANT) {
        LVPType result;
        if (isStride){
            result = loadClassificationTable->strideUpdate(tid, pc, classification, predicted_val == correct_val, correct_val - loadValuePredictionTable->getStride(tid, pc));
        }
        else if (isContext) { 
            result = loadClassificationTable->contextUpdate(tid, pc, classification, predicted_val == correct_val, loadValuePredictionTable->getVHTIndex(pc, tid) > 0);
        }
        else{
            result = loadClassificationTable->update(tid, pc, classification, predicted_val == correct_val);
        }
        if(result == LVP_CONSTANT) {
            DPRINTF(LVP, "[TID: %d] Load instruction 0x%x marked constant by LCT\n", tid, pc);
            constantVerificationUnit->updateConstLoad(pc, load_address, loadValuePredictionTable->getIndex(pc, tid), tid);
        }
    }
    return true;
}

std::pair<LVPType, RegVal>
LoadValuePredictionUnit::predictLoad(ThreadID tid, Addr pc) {
    DPRINTF(LVP, "Load Instruction: 0x%x being processed by LVPU\n", pc);
    std::pair<LVPType, RegVal> temp;
    LvptResult result = this->lookup(tid, pc);
    temp.first = result.taken;
    temp.second = result.value;
    return temp;
}

Addr
LoadValuePredictionUnit::lookupLVPTIndex(ThreadID tid, Addr pc) {
    return loadValuePredictionTable->getIndex(pc, tid);
}

bool
LoadValuePredictionUnit::processLoadAddress(ThreadID tid, Addr loadAddr, Addr pc) {

    Addr lvpt_index = loadValuePredictionTable->getIndex(pc, tid);
    return processLoadAddress(tid, loadAddr, pc, lvpt_index);
}

bool
LoadValuePredictionUnit::processLoadAddress(ThreadID tid, Addr loadAddr, Addr pc, Addr lvpt_index) {

    bool ret = constantVerificationUnit->processLoadAddress(pc, loadAddr, lvpt_index, tid);
    if(!ret) {
        // Load is not in the CVU CAM... Invalidate the const state in the LCT

        // Decrement counter or reset to 0?

        loadClassificationTable->update(tid, pc, LVP_CONSTANT, false);
        numConstLoadsMispredicted++;
    }
    else {
        numConstLoadsCorrect++;
    }
    return ret;
}

void
LoadValuePredictionUnit::startup()
{
    // Before simulation starts, we need to schedule the event
    DPRINTF(LVP, "Starting the LVP");
}

void
LoadValuePredictionUnit::regStats() {
    SimObject::regStats();

    numPredictableLoads.name(name() + ".numPredictableLoads")
                       .desc("Number of loads classified as predictable");

    numPredictableCorrect.name(name() + ".numPredictableLoadsCorrect")
                         .desc("Number of loads correctly classified as predictable");

    numPredictableIncorrect.name(name() + ".numPredictableLoadsIncorrect")
                           .desc("Number of loads incorrectly classified as predictable");

    numConstLoads.name(name() + ".numConstLoads")
                 .desc("Number of loads classified as constant");

    numConstLoadsMispredicted.name(name() + ".numConstLoadsMispredicted")
                             .desc("Number of constant loads incorrectly predicted");

    numConstLoadsCorrect.name(name() + ".numConstLoadsCorrect")
                             .desc("Number of constant loads correctly predicted");

    totalLoads.name(name() + ".totalLoads")
              .desc("Total loads processed by the Load value predictor");

    numZeroConstLoads.name(name() + ".numConstValZero")
                     .desc("Number of constant loads with value 0");

    numOneConstLoads.name(name() + ".numConstValOne")
                    .desc("Number of constant loads with value 1");
}

} // namespace gem5
