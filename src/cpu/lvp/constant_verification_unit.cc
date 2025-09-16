#include "cpu/lvp/constant_verification_unit.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CVU.hh"

namespace gem5{

ConstantVerificationUnit::ConstantVerificationUnit(const ConstantVerificationUnitParams &params) :
    SimObject(params), _numEntries(params.entries), _numConstantHits(0),
    _numConstantMiss(0), _numStoreHits(0), _numStoreMiss(0),
    _replacementPolicy(params.replacementPolicy), _numReplacements(0)
{}

ConstantVerificationUnit::~ConstantVerificationUnit() {}

void ConstantVerificationUnit::processStoreAddress(ThreadID tid,
													 Addr address) {
	DPRINTF(CVU, "[TID %d]: Store address: 0x%x being searched in CVU CAM\n", tid, address);
	// Only the load address needs to be compared with the store address
	bool found = false;
	auto itr = _cvuCAM[(uint16_t)tid%64].begin();
	while (itr != _cvuCAM[(uint16_t)tid%64].end()) {
		struct CAMEntry temp = *itr;
		if (temp.load_address == address) {
			DPRINTF(CVU, "[TID %d]: Found store address: 0x%x in CVU CAM\n", tid, address);
			itr = _cvuCAM[(uint16_t)tid%64].erase(itr);
			++_numStoreHits;
			found = true;
			if (itr == _cvuCAM[(uint16_t)tid%64].end()) break;
		}
		itr++;
	}
	if (!found) {
		DPRINTF(CVU, "[TID %d]: Address 0x%x not found in CVU CAM\n", tid, address);
		++_numStoreMiss;
	}
}

// I think we need the actual load address in here too
bool ConstantVerificationUnit::processLoadAddress(Addr pc,
													Addr loadAddr,
													Addr lvptIndex,
													ThreadID tid) {
	// Both load address and LVPT index have to be searched
	auto itr = _cvuCAM[(uint16_t)tid%64].begin();
	while (itr != _cvuCAM[(uint16_t)tid%64].end()) {
		struct CAMEntry temp = *itr;
		if (temp.pc == pc && temp.lvpt_index == lvptIndex && temp.load_address == loadAddr) {
			// print temp.load_addr and loadAddr
			// DPRINTF(CVU, "temp.load_address: 0x%x, loadAddr: 0x%x\n", temp.load_address, loadAddr);
			DPRINTF(CVU, "[TID %d] Load instruction: 0x%x matched in CVU CAM\n", tid, pc);
			++_numConstantHits;
			// Update the end time of this entry (for replacement purposes)
			itr->access = std::chrono::high_resolution_clock::now();
			return true;
		}
		itr++;
	}
	++_numConstantMiss;
	DPRINTF(CVU, "[TID %d] Load instruction: 0x%x not found in CVU CAM\n", tid, pc);
	return false;
}

bool ConstantVerificationUnit::updateConstLoad(Addr pc, Addr address,
											   Addr lvptIndex, ThreadID tid) {
	DPRINTF(CVU, "[TID %d]: Adding load address: 0x%x with PC: 0x%x to CVU CAM\n", tid, address, pc);
	struct CAMEntry temp;
	temp.pc = pc;
	temp.lvpt_index = lvptIndex;
	temp.load_address = address;
	temp.entry = std::chrono::high_resolution_clock::now();
	temp.access = temp.entry;
	if(_cvuCAM[(uint16_t)tid%64].size() >= _numEntries) {
		// CAM is full, replace a block according to a set replacement policy
		DPRINTF(CVU, "[TID %d]: No space for load address: 0x%x in CVU CAM\n", tid, address);
		this->replaceBlock(temp, tid);
		return true;
	}
	_cvuCAM[(uint16_t)tid%64].push_back(temp);
	return true;
}

void ConstantVerificationUnit::replaceBlock(struct CAMEntry new_entry, ThreadID tid) {
	_numReplacements++;
	if(_replacementPolicy == RP_FIFO) {
		// Kick the oldest entry from the CAM
		auto itr = _cvuCAM[(uint16_t)tid%64].begin();
		auto itr_replace = itr;
		TimePoint current_time = std::chrono::high_resolution_clock::now();
		double time_diff = DURATION(current_time - itr->entry).count();
		itr++;
		while(itr != _cvuCAM[(uint16_t)tid%64].end()) {
			if(time_diff < DURATION(current_time - itr->entry).count()) {
				itr_replace = itr;
				time_diff = DURATION(current_time - itr->entry).count();
			}
			itr++;
		}
		DPRINTF(CVU, "Entry with PC: 0x%x being ejected from CVU CAM\n", itr_replace->pc);
		_cvuCAM[(uint16_t)tid%64].erase(itr_replace);
		_cvuCAM[(uint16_t)tid%64].push_back(new_entry);
		return;
	}
	if(_replacementPolicy == RP_LRU) {
		// Kick the least recently used (compare the access)
		auto itr = _cvuCAM[(uint16_t)tid%64].begin();
		auto itr_replace = itr;
		TimePoint current_time = std::chrono::high_resolution_clock::now();
		double time_diff = DURATION(current_time - itr->access).count();
		itr++;
		while(itr != _cvuCAM[(uint16_t)tid%64].end()) {
			if(time_diff < DURATION(current_time - itr->access).count()) {
				itr_replace = itr;
				time_diff = DURATION(current_time - itr->access).count();
			}
			itr++;
		}
		DPRINTF(CVU, "Entry with PC: 0x%x being ejected from CVU CAM\n", itr_replace->pc);
		_cvuCAM[(uint16_t)tid%64].erase(itr_replace);
		_cvuCAM[(uint16_t)tid%64].push_back(new_entry);
		return;
	}
	if(_replacementPolicy == RP_NLRU) {
		// Kick the not least recently used (compare the access)
		auto itr = _cvuCAM[(uint16_t)tid%64].begin();
		auto itr_replace = itr;
		TimePoint current_time = std::chrono::high_resolution_clock::now();
		double time_diff = DURATION(current_time - itr->access).count();
		itr++;
		while(itr != _cvuCAM[(uint16_t)tid%64].end()) {
			if(time_diff < DURATION(current_time - itr->access).count()) {
				itr_replace = itr;
				time_diff = DURATION(current_time - itr->access).count();
			}
			itr++;
		}
		itr = _cvuCAM[(uint16_t)tid%64].begin();
		while(itr == itr_replace && itr != _cvuCAM[(uint16_t)tid%64].end()) {
			itr++;
		}
		itr_replace = itr;
		DPRINTF(CVU, "Entry with PC: 0x%x being ejected from CVU CAM\n", itr_replace->pc);
		_cvuCAM[(uint16_t)tid%64].erase(itr_replace);
		_cvuCAM[(uint16_t)tid%64].push_back(new_entry);
		return;
	}
	if(_replacementPolicy == RP_MRU) {
		// Kick the least recently used (compare the access)
		auto itr = _cvuCAM[(uint16_t)tid%64].begin();
		auto itr_replace = itr;
		TimePoint current_time = std::chrono::high_resolution_clock::now();
		double time_diff = DURATION(current_time - itr->access).count();
		itr++;
		while(itr != _cvuCAM[(uint16_t)tid%64].end()) {
			if(time_diff > DURATION(current_time - itr->access).count()) {
				itr_replace = itr;
				time_diff = DURATION(current_time - itr->access).count();
			}
			itr++;
		}
		DPRINTF(CVU, "Entry with PC: 0x%x being ejected from CVU CAM\n", itr_replace->pc);
		_cvuCAM[(uint16_t)tid%64].erase(itr_replace);
		_cvuCAM[(uint16_t)tid%64].push_back(new_entry);
		return;
	}
	if(_replacementPolicy == RP_NMRU) {
		// Kick the not least recently used (compare the access)
		auto itr = _cvuCAM[(uint16_t)tid%64].begin();
		auto itr_replace = itr;
		TimePoint current_time = std::chrono::high_resolution_clock::now();
		double time_diff = DURATION(current_time - itr->access).count();
		itr++;
		while(itr != _cvuCAM[(uint16_t)tid%64].end()) {
			if(time_diff > DURATION(current_time - itr->access).count()) {
				itr_replace = itr;
				time_diff = DURATION(current_time - itr->access).count();
			}
			itr++;
		}
		itr = _cvuCAM[(uint16_t)tid%64].begin();
		while(itr == itr_replace && itr != _cvuCAM[(uint16_t)tid%64].end()) {
			itr++;
		}
		itr_replace = itr;
		DPRINTF(CVU, "Entry with PC: 0x%x being ejected from CVU CAM\n", itr_replace->pc);
		_cvuCAM[(uint16_t)tid%64].erase(itr_replace);
		_cvuCAM[(uint16_t)tid%64].push_back(new_entry);
		return;
	}
}

void ConstantVerificationUnit::regStats() {
	SimObject::regStats();

	_numConstantHits.name(name() + ".constLoadHits")
					.desc("Number of loads marked constant that hit in the CVU CAM");
	_numConstantMiss.name(name() + ".constLoadMisses")
					.desc("Number of loads marked constant that missed in the CVU CAM");
	_numStoreHits.name(name() + ".numStoreHits")
				 .desc("Number of stores that hit in the CVU CAM");
	_numStoreMiss.name(name() + ".numStoreMisses")
				 .desc("Number of stores that missed in the CVU CAM");
	_numReplacements.name(name() + ".numCAMReplacements")
				    .desc("Number of CVU CAM entries replaced");
	_numStoreAccesses.name(name() + ".numStoreAccesses")
					 .desc("Number of store instructions which accessed the CVU");
	_numLoadAccesses.name(name() + ".numLoadAccesses")
					.desc("Number of loads marked constant which accessed the CVU");

	_numLoadAccesses = _numConstantMiss + _numConstantHits;
	_numStoreAccesses = _numStoreHits + _numStoreMiss;
}

} // namespace gem5
