/**
 * @defgroup   CONSTANT_VERIFICATION_UNIT constant verification unit
 *
 * @brief      This file the implements class of the constant verification unit
 * 			   for a load value predictor.
 * 			   The constant verification unit has the following functions:
 * 			   - Keep track of load addresses which have been deemed "constant"
 * 			   by the LCT
 * 			   - Monitor all committed stores and invalidate a constant load
 * 			   address if a store that is being committed has the same address
 * 			   as the constant load
 * 			   - Compare a load value for address that had been deemed
 * 			   "predictable" by the LCT with the actual load value retrieved
 * 			   from memory
 * 			   - Provide feedback to the LCT for all predictable loads
 * 			   - Possible implementation of this class could also include a
 * 			   mechanism to pass load values from the commit stage back to the
 * 			   LVPT (Since this will be needed if an instruction is to be
 * 			   promoted from unpredictable to predictable and so on.)
 */

#ifndef __CPU_LVP_CONSTANT_VERIFICATION_UNIT__
#define __CPU_LVP_CONSTANT_VERIFICATION_UNIT__

#include "sim/sim_object.hh"
#include "base/types.hh"
#include "params/ConstantVerificationUnit.hh"
#include "base/statistics.hh"

#include <vector>
#include <map>
#include <iterator>
#include <algorithm>
#include <chrono>

namespace gem5 {

// Replacement Policies
#define RP_FIFO 1        // CAM is implemented as a queue
#define RP_LRU  2 		 // The least recently used entry is kicked out
#define RP_NLRU 3 		 // The not least recently used entry is kicked out
#define RP_MRU  4        // The most recently used entry is kicked out
#define RP_NMRU 5        // The not most recently used entry is kicked out

#define DURATION std::chrono::duration_cast<std::chrono::duration<double, std::milli>>
typedef std::chrono::high_resolution_clock::time_point TimePoint;

/**
 * @brief      The CVU stores the load address and the index of the LVPT in a
 *             concatenated form in its CAM. This is searched by loads that are
 *             predicted as constant and by store addresses. Thus, the search of
 *             the CAM will be different for each case. The search for a load
 *             predicted as "constant" will be done on the entire entry in the
 *             CAM while a store address will only need to be compared with the
 *             load address stored in the CAM.
 */

/**
 * @brief      Data stored in every entry of the CVU CAM
 */
struct CAMEntry {
	Addr lvpt_index;
	Addr pc;
	Addr load_address;
	TimePoint entry;
	TimePoint access;
};

class ConstantVerificationUnit : public SimObject {
public:
	ConstantVerificationUnit(const ConstantVerificationUnitParams &p);

	~ConstantVerificationUnit();

	/**
	 * @brief      A store address will be provided to the CVU which will then
	 * 			   search the CAM and invalidate every entry which matches the
	 * 			   input address. This will also generate the control signals
	 * 			   required for communicating with the LCT.
	 *
	 * @param[in]  address  The store address
	 */
	void processStoreAddress(ThreadID tid, Addr address);

	/**
	 * @brief      Check if a load address classified as constant is present in
	 *  		   the CVU CAM.
	 *
	 * @param[in]  pc         The PC for this load instruction
	 * @param[in]  lvptIndex  The lvpt index corresponding to that load address
	 * @param[in]  tid        Thread ID
	 *
	 * @return     True if the load address, LVPT index pair exist in the CAM
	 * 			   False otherwise
	 */
	bool processLoadAddress(Addr pc, Addr loadAddr, Addr lvptIndex, ThreadID tid);

	/**
	 * @brief      Inserts info of a new constant load into the CVU CAM
	 *
	 * @param[in]  pc         Instruction address
	 * @param[in]  address    The load address
	 * @param[in]  lvptIndex  The lvpt index
	 * @param[in]  tid        The tid
	 *
	 * @return     True if the update is successful
	 */
	bool updateConstLoad(Addr pc, Addr address, Addr lvptIndex, ThreadID tid);

	/**
	 * @brief      Replaces an entry in the CVU CAM with a new one according to
	 *             a replacement policy
	 *
	 * @param[in]  new_entry  The new entry
	 * @param[in]  tid        The tid
	 */
	void replaceBlock(struct CAMEntry new_entry, ThreadID tid);

	/**
	 * @brief      Print stats
	 */
	void regStats() override;

private:
	/**
	 * The CVU Content Addressable Memory;
	 * If a store address is found in this memory, the corresponding entry is
	 * invalidated and the invalidation also triggers an update routine which
	 * tells the LCT that this load address is no longer constant.
	 * The vector stores an ordered pair (load address, LVPT index)
	 */
	std::list<struct CAMEntry> _cvuCAM[64];

	/**
	 * Number of entries in the CVU CAM
	 */
	uint32_t _numEntries;

	/**
	 * Number of loads marked "constant" which were incorrectly predicted.
	 */
	statistics::Scalar _numConstantHits;

	/**
	 * Number of loads marked "constant" that were correctly predicted.
	 */
	statistics::Scalar _numConstantMiss;

	/**
	 * Number of store addresses which hit in the CVU CAM
	 */
	statistics::Scalar _numStoreHits;

	/**
	 * Number of store addresses which missed in the CVU CAM.
	 */
	statistics::Scalar _numStoreMiss;

	/**
	 * Replacement policy of the CAM
	 */
	uint8_t _replacementPolicy;

	/**
	 * Number of CAM blocks replaced.
	 */
	statistics::Scalar _numReplacements;

	/**
	 * Total stores processed by the CVU
	 */
	statistics::Formula _numStoreAccesses;

	/**
	 * Total Loads processed by the CVU
	 */
	statistics::Formula _numLoadAccesses;
};


} // namespace gem5

#endif
