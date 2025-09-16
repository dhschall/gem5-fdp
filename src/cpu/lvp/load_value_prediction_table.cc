#include "cpu/lvp/load_value_prediction_table.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/LVPT.hh"

namespace gem5
{



LoadValuePredictionTable::LoadValuePredictionTable(const LoadValuePredictionTableParams &params)
    : SimObject(params),
      numEntries(params.entries),
      historyDepth(params.historyDepth),
      idxMask(numEntries - 1),
      instShiftAmt(0)

{
    DPRINTF(LVPT, "LVPT: Creating LVPT object.\n");

    if (!isPowerOf2(numEntries)) {
        fatal("LVPT entries is not a power of 2!");
    }

    LVPT.resize(numEntries);

    DPRINTF(LVPT, "LVPT: Doing an initial reset \n");
    for (unsigned i = 0; i < numEntries; ++i) {
        LVPT[i] = LVPTEntry(historyDepth);
    }

    idxMask = numEntries - 1;
    tagMask = (1 << tagBits) - 1;
    tagShiftAmt = instShiftAmt + floorLog2(numEntries);
}

/* Reset API */
void
LoadValuePredictionTable::reset()
{
    DPRINTF(LVPT, "LVPT : Calling the Reset API \n");
    for (unsigned i = 0; i < numEntries; ++i) {
        LVPT[i].valid = false;
    }
}

/* APIs to get index and tag*/
unsigned
LoadValuePredictionTable::getIndex(Addr instPC, ThreadID tid)
{
    // Need to shift PC over by the word offset.
    // Math: ((instPC >> instShiftAmt)^(tid<<(tagShiftAmt-instShiftAmt-log2NumThreads)))&idxMask;
    DPRINTF(LVPT, "LVPT : Getting Index \n");
    return ((instPC >> instShiftAmt)
            ^ (tid << (tagShiftAmt - instShiftAmt - log2NumThreads)))
            & idxMask;
}

inline
Addr
LoadValuePredictionTable::getTag(Addr instPC)
{
    DPRINTF(LVPT, "LVPT : Getting Tag \n");
    return (instPC >> tagShiftAmt) & tagMask;
}

/** Checks if the load entry is in the LVPT.i **/
bool
LoadValuePredictionTable::valid(Addr instPC, ThreadID tid)
{
    unsigned LVPT_idx = getIndex(instPC, tid);


    // Making sure index doesn't go out of bounds
    assert(LVPT_idx < numEntries);

    // Check if: (a) LVPT entry is valid
    // (b) index matches
    // (c) tag matches
    if (LVPT[LVPT_idx].valid
        && LVPT[LVPT_idx].tid == tid) {
        return true;
    } else {
        DPRINTF(LVPT, "LVPT : Checking if LVPT entry is valid \n");
        return false;
    }
}

// data = 0 represent invalid entry.
RegVal
LoadValuePredictionTable::lookup(ThreadID tid, Addr instPC, bool *lvptResultValid)
{
    unsigned LVPT_idx = getIndex(instPC, tid);

    assert(LVPT_idx < numEntries);

    if (valid(instPC, tid)) {
        DPRINTF(LVPT, "Found valid entry for tid: %d at pc %#x : %#x \n",
            tid, instPC, LVPT[LVPT_idx].history.back());
        *lvptResultValid = true;
        return LVPT[LVPT_idx].history.back();
    } else {
        DPRINTF(LVPT, "Did not find valid entry for tid: %d at address %#x \n",
            tid, instPC);
        return 0;
    }
}

void
LoadValuePredictionTable::strideInitialize()
{
    assert(historyDepth > 2);
    for (unsigned i = 0; i < numEntries; ++i) {
        LVPT[i].history.push_back(0);
        LVPT[i].history.push_back(1);
        LVPT[i].history.push_back(0);
    }
}

RegVal
LoadValuePredictionTable::strideLookup(ThreadID tid, Addr instPC, bool *lvptResultValid){
    unsigned LVPT_idx = getIndex(instPC, tid);

    assert(LVPT_idx < numEntries);
    assert(historyDepth > 2);

    if (valid(instPC, tid)) {
        DPRINTF(LVPT, "Found valid entry for tid: %d at pc %#x : %d \n",
        tid, instPC, LVPT[LVPT_idx].history.back());

        RegVal stride = LVPT[LVPT_idx].history.back() - LVPT[LVPT_idx].history[LVPT[LVPT_idx].history.size()-2];

        // if stride is same between last 3 values, then the result is valid
        *lvptResultValid = (LVPT[LVPT_idx].history[LVPT[LVPT_idx].history.size()-2] - LVPT[LVPT_idx].history[LVPT[LVPT_idx].history.size()-3] == stride);

        return LVPT[LVPT_idx].history.back() + stride;
        

    } else {
        DPRINTF(LVPT, "Did not find valid entry for tid: %d at address %#x \n",
            tid, instPC);
        return 0;
    }
}

RegVal
LoadValuePredictionTable::getStride(ThreadID tid, Addr instPC){
    unsigned LVPT_idx = getIndex(instPC, tid);

    assert(LVPT_idx < numEntries);
    assert(historyDepth > 2);

    if (valid(instPC, tid)) {
        if (LVPT[LVPT_idx].history.size() < 2) {
            return 0;
        }
        return LVPT[LVPT_idx].history.back() - LVPT[LVPT_idx].history[LVPT[LVPT_idx].history.size()-2];
    } else {
        return 0;
    }
}

RegVal
LoadValuePredictionTable::hashContext(const std::deque<RegVal> &context)
{
    unsigned hash = 0;
    for (auto val : context) {
        hash ^= val + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash & idxMask;
}

/* Get VHT Index */
RegVal
LoadValuePredictionTable::getVHTIndex(Addr instPC, ThreadID tid)
{
    return (instPC ^ tid) & idxMask;
}

RegVal
LoadValuePredictionTable::contextLookup(ThreadID tid, Addr instPC, bool *lvptResultValid){
    unsigned VHT_idx = getVHTIndex(instPC, tid); // change to uint64?

    // P1) Index into the VHT
    if (VHT.find(VHT_idx) == VHT.end()) {
        // If no valid entry exists in VHT, return default prediction
        DPRINTF(LVPT, "No VHT entry for PC %#x and tid %d\n", instPC, tid);
        *lvptResultValid = false;
        return 0;
    }

    VHTEntry &vhtEntry = VHT[VHT_idx];
    const auto &context = vhtEntry.context;

    // P2) Get an index for the VPT to get the prediction. Save the returned context into the queue
    unsigned VPT_idx = hashContext(context);

    if (VPT.find(VPT_idx) == VPT.end()) {
        // If no valid entry exists in VPT, return 0
        DPRINTF(LVPT, "No VPT entry for context at PC %#x and tid %d\n", instPC, tid);
        *lvptResultValid = false;
        return 0;
    }

    // Getting the entry at the right location in the VPT
    VPTEntry &vptEntry = VPT[VPT_idx];

    // P3) Form prediction from value in the VPT
    DPRINTF(LVPT, "Prediction for PC %#x: %lu (confidence: %d)\n",
            instPC, vptEntry.prediction, vptEntry.confidence);

    // Update VHT with predicted value
    vhtEntry.context.push_back(vptEntry.prediction);
    if (vhtEntry.context.size() > historyDepth) {
        vhtEntry.context.pop_front(); // Maintain order-k context
    }

    *lvptResultValid = (vptEntry.confidence > confidenceThreshold);
    return vptEntry.prediction;

}

/* Update VHT and VPT */
void
LoadValuePredictionTable::contextUpdate(Addr instPC, const RegVal correctValue, ThreadID tid)
{
    unsigned VHT_idx = getVHTIndex(instPC, tid);
    DPRINTF(LVPT, "Updating VHT for PC %#x, tid %d with value %lu\n", instPC, tid, correctValue);

    // U1) Retrieve saved context from update queue when correct value available
    // Update VHT
    if (VHT.find(VHT_idx) == VHT.end()) {
        VHT[VHT_idx] = VHTEntry(historyDepth);
    }
    auto &context = VHT[VHT_idx].context;

    // Use context to index into VPT
    unsigned VPT_idx = hashContext(context);

    if (VPT.find(VPT_idx) == VPT.end()) {
        VPT[VPT_idx] = VPTEntry();
    }
    auto &vptEntry = VPT[VPT_idx];

    // U2) Update VHT and VPT using the correct value
    // Update prediction value and confidence
    if (vptEntry.prediction == correctValue) {
        vptEntry.confidence = std::min(vptEntry.confidence + 1, 255);
    } else {
        vptEntry.confidence = std::max(vptEntry.confidence - 1, 0);
    }
    vptEntry.prediction = correctValue;

    // Update VHT with correct value
    context.push_back(correctValue);
    if (context.size() > historyDepth) {
        context.pop_front();
    }

    DPRINTF(LVPT, "Updated VHT and VPT for PC %#x, tid %d\n", instPC, tid);
}


void
//prajyotg :: updated :: LoadValuePredictionTable::update(Addr instPC, const TheISA::PCState &target, ThreadID tid)
LoadValuePredictionTable::update(Addr instPC, const RegVal target, ThreadID tid)
{
    unsigned LVPT_idx = getIndex(instPC, tid);
    DPRINTF(LVPT, "LVPT : Updating the value in the LVPT at index %ld \n", LVPT_idx);

    assert(LVPT_idx < numEntries);

    LVPT[LVPT_idx].tid = tid;
    LVPT[LVPT_idx].valid = true;
    LVPT[LVPT_idx].history.push_back(target);
    LVPT[LVPT_idx].tag = getTag(instPC);
}

// Get this real method from Roshan
RegVal
LoadValuePredictionTable::getContext(ThreadID tid, Addr instPC)
{
    return RegVal(0);
}
} // namespace gem5
