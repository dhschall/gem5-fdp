#ifndef __CPU_LVP_LOADVALUEPREDICTIONTABLE_HH__
#define __CPU_LVP_LOADVALUEPREDICTIONTABLE_HH__

#include "base/types.hh"
#include "cpu/static_inst.hh"
#include "sim/sim_object.hh"

#include "enums.hh"
#include "params/LoadValuePredictionTable.hh"

#include "base/types.hh"
#include "base/logging.hh"

#include <boost/circular_buffer.hpp>
#include <queue>

/** Creating a default Load Value Prediction Table entry
 *  which will have below attributes
 *  tag   : Specifies the Opcode of the Load instruction
 *  taget : Specifies the Load value associated with the tag
 *  valid : Specifies if the value loaded is valid
 */

namespace gem5
{
class LoadValuePredictionTable : public SimObject
{
  protected:
    struct LVPTEntry
    {
        LVPTEntry()
            : tag(0), history(), tid(0), valid(false)
        {}

        LVPTEntry(size_t depth)
            : tag(0), history(depth), tid(0), valid(false)
        {}

        LVPTEntry(const LVPTEntry& e) = default;

        /** The entry's tag. */
        Addr tag;

        /** The entry's history. */
        boost::circular_buffer<RegVal> history;

        /** The entry's thread id. */
        ThreadID tid;

        /** Whether or not the entry is valid. */
        bool valid;
    };

    struct VHTEntry {
        VHTEntry()
            : context(2), confidence(0)
        {}
        VHTEntry(int depth)
            : context(depth), confidence(0)
        {}
        std::deque<uint64_t> context; // k previous vals
        int confidence; // Conf. Counter
    };

    struct VPTEntry {
        uint64_t prediction; // Predicted value
        int confidence; // Conf. Counter
    };

    std::unordered_map<uint64_t, VHTEntry> VHT; // VHT
    std::unordered_map<uint64_t, VPTEntry> VPT; // VPT

    std::queue<std::tuple<uint64_t, std::deque<uint64_t>, uint64_t>> updateQueue; // Update Queue

  public:
    /** Creates a LVPT with the given number of entries, number of bits per
     *  tag, and instruction offset amount.
     *  @param numEntries Number of entries for the LVPT.
     *  @param tagBits Number of bits for each tag in the LVPT.
     *  @param instShiftAmt Offset amount for instructions to ignore alignment.
     */
    LoadValuePredictionTable(const LoadValuePredictionTableParams &params);

    void reset();

    /** Looks up an address in the LVPT. Must call valid() first on the address.
     *  @param inst_PC The address of the branch to look up.
     *  @param tid The thread id.
     *  @return Returns the predicated load value.
     */
    RegVal lookup(ThreadID tid, Addr instPC, bool *lvptResultValid);

    void strideInitialize();

    RegVal strideLookup(ThreadID tid, Addr instPC, bool *lvptResultValid);

    RegVal getStride(ThreadID tid, Addr instPC);

    RegVal contextLookup(ThreadID tid, Addr instPC, bool *lvptResultValid);

    RegVal getContext(ThreadID tid, Addr instPC);

    /** Checks if the load entry is in the LVPT.
     *  @param inst_PC The address of the branch to look up.
     *  @param tid The thread id.
     *  @return Whether or not the branch exists in the LVPT.
     */
    bool valid(Addr instPC, ThreadID tid);

    /** Updates the LVPT with the latest predicted Load Value.
     *  @param inst_PC The address of the branch being updated.
     *  @param target_PC The predicted target data.
     *  @param tid The thread id.
     */

    void update(Addr instPC, const RegVal target, ThreadID tid);

    /** Returns the index into the LVPT, based on the branch's PC.
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the LVPT.
     */

    unsigned getIndex(Addr instPC, ThreadID tid);

    RegVal hashContext(const std::deque<RegVal> &context);

    RegVal getVHTIndex(Addr instPC, ThreadID tid);

    void contextUpdate(Addr instPC, const RegVal correctValue, ThreadID tid);


    /** Returns the tag bits of a given address.
     *  @param inst_PC The branch's address.
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr instPC);

    int confidenceThreshold = 2;

  protected:

    /** The actual LVPT declaration */
    std::vector<LVPTEntry> LVPT;

    /** The number of entries in the LVPT. */
    const unsigned numEntries;

    /** Depth of data history kept in the LVPT*/
    const unsigned historyDepth;

    //prajyotg :: No sure if below will be used
    /** The index mask. */
    unsigned idxMask;

    /** The number of tag bits per entry. */
    unsigned tagBits;

    /** The tag mask. */
    unsigned tagMask;

    /** Number of bits to shift PC when calculating index. */
    unsigned instShiftAmt;

    /** Number of bits to shift PC when calculating tag. */
    unsigned tagShiftAmt;

    /** Log2 NumThreads used for hashing threadid */
    unsigned log2NumThreads;
};

} // namespace gem5

#endif // __CPU_LVP_LOADVALUEPREDICTIONTABLE_HH__
