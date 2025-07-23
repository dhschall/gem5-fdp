/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/mem_dep_unit.hh"

#include <map>
#include <memory>
#include <vector>

#include "base/compiler.hh"
#include "base/debug.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/inst_queue.hh"
#include "cpu/o3/limits.hh"
#include "debug/MemDepUnit.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

#ifdef GEM5_DEBUG
int MemDepUnit::MemDepEntry::memdep_count = 0;
int MemDepUnit::MemDepEntry::memdep_insert = 0;
int MemDepUnit::MemDepEntry::memdep_erase = 0;
#endif

MemDepUnit::MemDepUnit() : iqPtr(NULL), stats(nullptr) {}

MemDepUnit::MemDepUnit(const BaseO3CPUParams &params)
    : _name(params.name + ".memdepunit"),
      depPred(params, this),
      iqPtr(NULL),
      stats(nullptr)
{
    DPRINTF(MemDepUnit, "Creating MemDepUnit object.\n");
}

MemDepUnit::~MemDepUnit()
{
    for (ThreadID tid = 0; tid < MaxThreads; tid++) {

        ListIt inst_list_it = instList[tid].begin();

        MemDepHashIt hash_it;

        while (!instList[tid].empty()) {
            hash_it = memDepHash.find((*inst_list_it)->seqNum);

            assert(hash_it != memDepHash.end());

            memDepHash.erase(hash_it);

            instList[tid].erase(inst_list_it++);
        }
    }

#ifdef GEM5_DEBUG
    assert(MemDepEntry::memdep_count == 0);
#endif
}

void
MemDepUnit::init(const BaseO3CPUParams &params, ThreadID tid, CPU *_cpu)
{
    DPRINTF(MemDepUnit, "Creating MemDepUnit %i object.\n",tid);

    _name = csprintf("%s.memDep%d", params.name, tid);
    id = tid;
    cpu = _cpu;

    depPred.init(params, this);

    std::string stats_group_name = csprintf("MemDepUnit__%i", tid);
    cpu->addStatGroup(stats_group_name.c_str(), &stats);
}

MemDepUnit::MemDepUnitStats::MemDepUnitStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(insertedLoads, statistics::units::Count::get(),
               "Number of loads inserted to the mem dependence unit."),
      ADD_STAT(insertedStores, statistics::units::Count::get(),
               "Number of stores inserted to the mem dependence unit."),
      ADD_STAT(conflictingLoads, statistics::units::Count::get(),
               "Number of conflicting loads."),
      ADD_STAT(conflictingStores, statistics::units::Count::get(),
               "Number of conflicting stores."),
      /** ==== Store Set ==== */
      ADD_STAT(LFSTReads, statistics::units::Count::get(),
               "Number of LFST reads."),
      ADD_STAT(LFSTWrites, statistics::units::Count::get(),
               "Number of LFST writes."),
      /** ==== PHAST ==== */
      ADD_STAT(falseDependencies, statistics::units::Count::get(),
               "Number of times load's address didn't match predicted store's address"),
      ADD_STAT(correctPredictions, statistics::units::Count::get(),
               "Number of times load's address did match predicted store's address"),
      ADD_STAT(readsPath1, statistics::units::Count::get(),
               "Number of reads to path table 1."),
      ADD_STAT(readsPath2, statistics::units::Count::get(),
               "Number of reads to path table 2."),
      ADD_STAT(readsPath3, statistics::units::Count::get(),
               "Number of reads to path table 3."),
      ADD_STAT(readsPath4, statistics::units::Count::get(),
               "Number of reads to path table 4."),
      ADD_STAT(readsPath5, statistics::units::Count::get(),
               "Number of reads to path table 5."),
      ADD_STAT(readsPath6, statistics::units::Count::get(),
               "Number of reads to path table 6."),
      ADD_STAT(readsPath7, statistics::units::Count::get(),
               "Number of reads to path table 7."),
      ADD_STAT(readsPath8, statistics::units::Count::get(),
               "Number of reads to path table 8."),
      ADD_STAT(writesPath1, statistics::units::Count::get(),
               "Number of writes to path table 1."),
      ADD_STAT(writesPath2, statistics::units::Count::get(),
               "Number of writes to path table 2."),
      ADD_STAT(writesPath3, statistics::units::Count::get(),
               "Number of writes to path table 3."),
      ADD_STAT(writesPath4, statistics::units::Count::get(),
               "Number of writes to path table 4."),
      ADD_STAT(writesPath5, statistics::units::Count::get(),
               "Number of writes to path table 5."),
      ADD_STAT(writesPath6, statistics::units::Count::get(),
               "Number of writes to path table 6."),
      ADD_STAT(writesPath7, statistics::units::Count::get(),
               "Number of writes to path table 7."),
      ADD_STAT(writesPath8, statistics::units::Count::get(),
               "Number of writes to path table 8.")
{
}

bool
MemDepUnit::isDrained() const
{
    bool drained = instsToReplay.empty()
                 && memDepHash.empty()
                 && instsToReplay.empty();
    for (int i = 0; i < MaxThreads; ++i)
        drained = drained && instList[i].empty();

    return drained;
}

void MemDepUnit::clear_dep_pred() { depPred.clear(); }

void
MemDepUnit::drainSanityCheck() const
{
    assert(instsToReplay.empty());
    assert(memDepHash.empty());
    for (int i = 0; i < MaxThreads; ++i)
        assert(instList[i].empty());
    assert(instsToReplay.empty());
    assert(memDepHash.empty());
}

void
MemDepUnit::takeOverFrom()
{
    // Be sure to reset all state.
    loadBarrierSNs.clear();
    storeBarrierSNs.clear();
    depPred.clear();
}

void
MemDepUnit::setIQ(InstructionQueue *iq_ptr)
{
    iqPtr = iq_ptr;
}

void
MemDepUnit::insertBarrierSN(const DynInstPtr &barr_inst)
{
    InstSeqNum barr_sn = barr_inst->seqNum;

    if (barr_inst->isReadBarrier() || barr_inst->isHtmCmd())
        loadBarrierSNs.insert(barr_sn);
    if (barr_inst->isWriteBarrier() || barr_inst->isHtmCmd())
        storeBarrierSNs.insert(barr_sn);

    if (debug::MemDepUnit) {
        const char *barrier_type = nullptr;
        if (barr_inst->isReadBarrier() && barr_inst->isWriteBarrier())
            barrier_type = "memory";
        else if (barr_inst->isReadBarrier())
            barrier_type = "read";
        else if (barr_inst->isWriteBarrier())
            barrier_type = "write";

        if (barrier_type) {
            DPRINTF(MemDepUnit, "Inserted a %s barrier %s SN:%lli\n",
                    barrier_type, barr_inst->pcState(), barr_sn);
        }

        if (loadBarrierSNs.size() || storeBarrierSNs.size()) {
            DPRINTF(MemDepUnit, "Outstanding load barriers = %d; "
                                "store barriers = %d\n",
                    loadBarrierSNs.size(), storeBarrierSNs.size());
        }
    }
}

void
MemDepUnit::insert(const DynInstPtr &inst, BranchHistory branchHistory)
{
    ThreadID tid = inst->threadNumber;

    MemDepEntryPtr inst_entry = std::make_shared<MemDepEntry>(inst);

    // Add the MemDepEntry to the hash.
    memDepHash.insert(
        std::pair<InstSeqNum, MemDepEntryPtr>(inst->seqNum, inst_entry));
#ifdef GEM5_DEBUG
    MemDepEntry::memdep_insert++;
#endif

    instList[tid].push_back(inst);

    inst_entry->listIt = --(instList[tid].end());

    std::vector<MemDepEntryPtr> dependencies;
    PredictionResult prediction;
    prediction.storeQueueDistance = 0;
    prediction.seqNum = 0;
    prediction = depPred.checkInst(inst->pcState().instAddr(), inst->seqNum, branchHistory, inst->isLoad());

    if (prediction.storeQueueDistance && inst->sqIt.idx() >= (cpu->getIEW()->ldstQueue.getStoreHead(id) + prediction.storeQueueDistance)){
        //make a PHAST prediction, as long as the SQ offset is valid
        auto sq_it = inst->sqIt - prediction.storeQueueDistance;
        DynInstPtr store_inst = sq_it->instruction();
        MemDepHashIt hash_it = memDepHash.find(store_inst->seqNum);

        if (hash_it != memDepHash.end()) {
            dependencies.push_back((*hash_it).second);
            inst->memDepInfo.predBranchHistLength = prediction.predBranchHistLength;
            inst->memDepInfo.predictorHash = prediction.predictorHash;
            inst->memDepInfo.predicted = true;
        }
    } else if (prediction.seqNum) {
        //make a StoreSet prediction
        MemDepHashIt hash_it = memDepHash.find(prediction.seqNum);

        if (hash_it != memDepHash.end()) {
            dependencies.push_back((*hash_it).second);
            inst->memDepInfo.predicted = true;
        }
    }

    /* 2nd Step: Concurrently, check the in-flight Barriers; the Load
       and Store Instructions are not allowed to overtake the
       corresponding Barriers. (i.e., Comply with Consistency) */
    if ((inst->isLoad() || inst->isAtomic()) && hasLoadBarrier()) {
        DPRINTF(MemDepUnit, "%d load barriers in flight\n",
                loadBarrierSNs.size());
        for (InstSeqNum sn: loadBarrierSNs) {
            MemDepHashIt hash_it = memDepHash.find(sn);

            if (hash_it != memDepHash.end()) {
                dependencies.push_back((*hash_it).second);
                DPRINTF(MemDepUnit, "LoadBarrier found in HashMap.\n");
            }
        }
    }

    /** For Atomic Instruction it should be dependent on
    both LoadBarrier & StoreBarrier. */
    if ((inst->isStore() || inst->isAtomic()) && hasStoreBarrier()) {
        DPRINTF(MemDepUnit, "%d store barriers in flight\n",
                storeBarrierSNs.size());
        for (InstSeqNum sn: storeBarrierSNs) {
            MemDepHashIt hash_it = memDepHash.find(sn);

            if (hash_it != memDepHash.end()) {
                dependencies.push_back((*hash_it).second);
                DPRINTF(MemDepUnit, "StoreBarrier found in HashMap.\n");
            }
        }
    }

    /* If there are not any dependencies (i.e., the Instruction is not
       dependent on any Inst), then Instruction can be issued as
       soon as the registers are ready. */
    if (dependencies.empty()) {
        DPRINTF(MemDepUnit, "No dependency for inst PC "
                "%s [sn:%lli].\n", inst->pcState(), inst->seqNum);

        /* The Counter "memDependencies" for the inst_entry is by default zero;
           So there is no need to do sth here, like enabling any flags. */

        if (inst->readyToIssue()) {
            inst_entry->regsReady = true;

            moveToReady(inst_entry);

            DPRINTF(MemDepUnit, "Also the Inst is ready to issue.\n");
        }
    } else {
        /* The current Instruction has some dependencies;
           either Store or Barriers or Both. */
        inst_entry->memDeps = dependencies.size();

        /* Append the instruction in the dependent_VectorList of
           each dependency that has been found.*/
        for (const auto &dependency: dependencies) {
            DPRINTF(MemDepUnit, "Adding to dependency list; "
                    "inst PC %s [sn:%lli] is dependent on [sn:%lli].\n",
                    inst->pcState(), inst->seqNum,
                    dependency->inst->seqNum);

            dependency->dependInsts.push_back(inst_entry);
        }

        /* If the Instruction is ready_to_Issue, we only set the
           flag; We are not allowed to issue the Instruction until
           all the dependencies have been resolved. */
        if (inst->readyToIssue()) {
            inst_entry->regsReady = true;
        }

        // Clear the bit saying this instruction can issue.
        inst->clearCanIssue();

        if (inst->isLoad()) {
            ++stats.conflictingLoads;
        } else {
            ++stats.conflictingStores;
        }
    }

    insertBarrierSN(inst);

    if (inst->isStore() || inst->isAtomic()) {
        DPRINTF(MemDepUnit, "Inserting store/atomic PC %s [sn:%lli].\n",
                inst->pcState(), inst->seqNum);

        depPred.insertStore(inst->pcState().instAddr(), inst->seqNum,
                            inst->threadNumber);

        ++stats.insertedStores;
    } else if (inst->isLoad()) {
        ++stats.insertedLoads;
    } else {
        panic("Unknown type! (most likely a barrier).");
    }
}

void
MemDepUnit::insertNonSpec(const DynInstPtr &inst)
{
    insertBarrier(inst);

    // Might want to turn this part into an inline function or something.
    // It's shared between both insert functions.
    if (inst->isStore() || inst->isAtomic()) {
        DPRINTF(MemDepUnit, "Inserting store/atomic PC %s [sn:%lli].\n",
                inst->pcState(), inst->seqNum);

        depPred.insertStore(inst->pcState().instAddr(), inst->seqNum,
                inst->threadNumber);

        ++stats.insertedStores;
    } else if (inst->isLoad()) {
        ++stats.insertedLoads;
    } else {
        panic("Unknown type! (most likely a barrier).");
    }
}

void
MemDepUnit::insertBarrier(const DynInstPtr &barr_inst)
{
    ThreadID tid = barr_inst->threadNumber;

    MemDepEntryPtr inst_entry = std::make_shared<MemDepEntry>(barr_inst);

    // Add the MemDepEntry to the hash.
    memDepHash.insert(
        std::pair<InstSeqNum, MemDepEntryPtr>(barr_inst->seqNum, inst_entry));
#ifdef GEM5_DEBUG
    MemDepEntry::memdep_insert++;
#endif

    // Add the instruction to the instruction list.
    instList[tid].push_back(barr_inst);

    inst_entry->listIt = --(instList[tid].end());

    insertBarrierSN(barr_inst);
}

void
MemDepUnit::regsReady(const DynInstPtr &inst)
{
    DPRINTF(MemDepUnit, "Marking registers as ready for "
            "instruction PC %s [sn:%lli].\n",
            inst->pcState(), inst->seqNum);

    MemDepEntryPtr inst_entry = findInHash(inst);

    inst_entry->regsReady = true;

    if (inst_entry->memDeps == 0) {
        DPRINTF(MemDepUnit, "Instruction has its memory "
                "dependencies resolved, adding it to the ready list.\n");

        moveToReady(inst_entry);
    } else {
        DPRINTF(MemDepUnit, "Instruction PC %#x [sn:%lli] still "
                "waiting on memory dependency.\n",
                inst_entry->inst->pcState().instAddr(), inst_entry->inst->seqNum);
    }
}

void
MemDepUnit::nonSpecInstReady(const DynInstPtr &inst)
{
    DPRINTF(MemDepUnit, "Marking non speculative "
            "instruction PC %s as ready [sn:%lli].\n",
            inst->pcState(), inst->seqNum);

    MemDepEntryPtr inst_entry = findInHash(inst);

    moveToReady(inst_entry);
}

void
MemDepUnit::reschedule(const DynInstPtr &inst)
{
    instsToReplay.push_back(inst);
}

void
MemDepUnit::replay()
{
    DynInstPtr temp_inst;

    // For now this replay function replays all waiting memory ops.
    while (!instsToReplay.empty()) {
        temp_inst = instsToReplay.front();

        MemDepEntryPtr inst_entry = findInHash(temp_inst);

        DPRINTF(MemDepUnit, "Replaying mem instruction PC %s [sn:%lli].\n",
                temp_inst->pcState(), temp_inst->seqNum);

        moveToReady(inst_entry);

        instsToReplay.pop_front();
    }
}

void
MemDepUnit::completed(const DynInstPtr &inst)
{
    DPRINTF(MemDepUnit, "Completed mem instruction PC %s [sn:%lli].\n",
            inst->pcState(), inst->seqNum);

    ThreadID tid = inst->threadNumber;

    // Remove the instruction from the hash and the list.
    MemDepHashIt hash_it = memDepHash.find(inst->seqNum);

    assert(hash_it != memDepHash.end());

    instList[tid].erase((*hash_it).second->listIt);

    (*hash_it).second = NULL;

    memDepHash.erase(hash_it);
#ifdef GEM5_DEBUG
    MemDepEntry::memdep_erase++;
#endif
}

void
MemDepUnit::completeInst(const DynInstPtr &inst)
{
    wakeDependents(inst);
    completed(inst);
    InstSeqNum barr_sn = inst->seqNum;

    if (inst->isWriteBarrier() || inst->isHtmCmd()) {
        assert(hasStoreBarrier());
        storeBarrierSNs.erase(barr_sn);
    }
    if (inst->isReadBarrier() || inst->isHtmCmd()) {
        assert(hasLoadBarrier());
        loadBarrierSNs.erase(barr_sn);
    }
    if (debug::MemDepUnit) {
        const char *barrier_type = nullptr;
        if (inst->isWriteBarrier() && inst->isReadBarrier())
            barrier_type = "Memory";
        else if (inst->isWriteBarrier())
            barrier_type = "Write";
        else if (inst->isReadBarrier())
            barrier_type = "Read";

        if (barrier_type) {
            DPRINTF(MemDepUnit, "%s barrier completed: %s SN:%lli\n",
                                barrier_type, inst->pcState(), inst->seqNum);
        }
    }
}

void
MemDepUnit::wakeDependents(const DynInstPtr &inst)
{
    // Only stores, atomics and barriers have dependents.
    if (!inst->isStore() && !inst->isAtomic() && !inst->isReadBarrier() &&
        !inst->isWriteBarrier() && !inst->isHtmCmd()) {
        return;
    }

    MemDepEntryPtr inst_entry = findInHash(inst);

    /* By entering to this function, we release one dependency for
       the dependent_inst
       (Inst which was stopped from issuing due to one or
       multiple dependencies.)

       --> !! The dependent_inst is going to be woken up whenever
       all the dependencies have been resolved. !!
    */

    for (int i = 0; i < inst_entry->dependInsts.size(); ++i ) {
        MemDepEntryPtr dependent_inst = inst_entry->dependInsts[i];

        if (!dependent_inst->inst) {
            // Potentially removed mem dep entries could be on this list
            continue;
        }

        DPRINTF(MemDepUnit, "Inst PC: %#x [sn:%lli] is Releasing one "
        "dependency for inst PC: %#x [sn:%lli].\n",
        inst->pcState().instAddr(), inst->seqNum,
        dependent_inst->inst->pcState().instAddr(),
        dependent_inst->inst->seqNum);

        // release one dependency.
        dependent_inst->memDeps--;

        if (dependent_inst->memDeps == 0) {
            if (dependent_inst->inst->memDepInfo.predicted && inst->isStore()) {
                dependent_inst->inst->memDepInfo.predStoreAddr = inst->effAddr;
                dependent_inst->inst->memDepInfo.predStoreSize = inst->effSize;
            }
            if (dependent_inst->regsReady && !dependent_inst->squashed) {
                DPRINTF(MemDepUnit, "Inst PC: %#x [sn:%lli] is just "
                        "woken up!!\n",
                        dependent_inst->inst->pcState().instAddr(),
                        dependent_inst->inst->seqNum);

                /** All the dependencies have been resolved & the
                    Registers are ready as well. */
                moveToReady(dependent_inst);
            }
        }
    }

    inst_entry->dependInsts.clear();
}

MemDepUnit::MemDepEntry::MemDepEntry(const DynInstPtr &new_inst) :
    inst(new_inst)
{
#ifdef GEM5_DEBUG
    ++memdep_count;

    DPRINTF(MemDepUnit,
            "Memory dependency entry created. memdep_count=%i %s\n",
            memdep_count, inst->pcState());
#endif
}

MemDepUnit::MemDepEntry::~MemDepEntry()
{
    for (int i = 0; i < dependInsts.size(); ++i) {
        dependInsts[i] = NULL;
    }
#ifdef GEM5_DEBUG
    --memdep_count;

    DPRINTF(MemDepUnit,
            "Memory dependency entry deleted. memdep_count=%i %s\n",
            memdep_count, inst->pcState());
#endif
}

void
MemDepUnit::squash(const InstSeqNum &squashed_num, ThreadID tid)
{
    if (!instsToReplay.empty()) {
        ListIt replay_it = instsToReplay.begin();
        while (replay_it != instsToReplay.end()) {
            if ((*replay_it)->threadNumber == tid &&
                (*replay_it)->seqNum > squashed_num) {
                instsToReplay.erase(replay_it++);
            } else {
                ++replay_it;
            }
        }
    }

    ListIt squash_it = instList[tid].end();
    --squash_it;

    MemDepHashIt hash_it;

    while (!instList[tid].empty() &&
           (*squash_it)->seqNum > squashed_num) {

        DPRINTF(MemDepUnit, "Squashing inst [sn:%lli]\n",
                (*squash_it)->seqNum);

        loadBarrierSNs.erase((*squash_it)->seqNum);

        storeBarrierSNs.erase((*squash_it)->seqNum);

        hash_it = memDepHash.find((*squash_it)->seqNum);

        assert(hash_it != memDepHash.end());

        (*hash_it).second->squashed = true;

        (*hash_it).second = NULL;

        memDepHash.erase(hash_it);
#ifdef GEM5_DEBUG
        MemDepEntry::memdep_erase++;
#endif

        instList[tid].erase(squash_it--);
    }

    // Tell the dependency predictor to squash as well.
    depPred.squash(squashed_num, tid);
}

void
MemDepUnit::violation(InstSeqNum store_seq_num, Addr store_pc,
        const DynInstPtr &violating_load, BranchHistory branchHistory)
{
    DPRINTF(MemDepUnit, "Passing violating PCs to store sets,"
            " load: %#x, store seq num: %#d\n", violating_load->pcState().instAddr(),
            store_seq_num);
    // Tell the memory dependence unit of the violation.
    depPred.violation(violating_load->pcState().instAddr(), violating_load->seqNum, store_seq_num, store_pc,
                      violating_load->memDepInfo.storeQueueDistance, violating_load->memDepInfo.predicted,
                      violating_load->memDepInfo.predBranchHistLength,
                      violating_load->memDepInfo.predictorHash, branchHistory);
}

void
MemDepUnit::issue(const DynInstPtr &inst)
{
    DPRINTF(MemDepUnit, "Issuing instruction PC %#x [sn:%lli].\n",
            inst->pcState().instAddr(), inst->seqNum);

    depPred.issued(inst->pcState().instAddr(), inst->seqNum, inst->isStore());
}

void
MemDepUnit::commit(const DynInstPtr &inst)
{
    DPRINTF(MemDepUnit, "Committing instruction PC %#x [sn:%lli].\n",
            inst->pcState().instAddr(), inst->seqNum);

    if (inst->isStore()) return;

    depPred.commit(inst->pcState().instAddr(), inst->effAddr,
                   inst->effSize, inst->memDepInfo.predStoreAddr, inst->memDepInfo.predStoreSize,
                   inst->memDepInfo.predBranchHistLength, inst->memDepInfo.predictorHash);
}

MemDepUnit::MemDepEntryPtr &
MemDepUnit::findInHash(const DynInstConstPtr &inst)
{
    MemDepHashIt hash_it = memDepHash.find(inst->seqNum);

    assert(hash_it != memDepHash.end());

    return (*hash_it).second;
}

void
MemDepUnit::moveToReady(MemDepEntryPtr &woken_inst_entry)
{
    DPRINTF(MemDepUnit, "Adding instruction [sn:%lli] "
            "to the ready list.\n", woken_inst_entry->inst->seqNum);

    assert(!woken_inst_entry->squashed);

    iqPtr->addReadyMemInst(woken_inst_entry->inst);
}


void
MemDepUnit::dumpLists()
{
    for (ThreadID tid = 0; tid < MaxThreads; tid++) {
        cprintf("Instruction list %i size: %i\n",
                tid, instList[tid].size());

        ListIt inst_list_it = instList[tid].begin();
        int num = 0;

        while (inst_list_it != instList[tid].end()) {
            cprintf("Instruction:%i\nPC: %s\n[sn:%llu]\n[tid:%i]\nIssued:%i\n"
                    "Squashed:%i\n\n",
                    num, (*inst_list_it)->pcState(),
                    (*inst_list_it)->seqNum,
                    (*inst_list_it)->threadNumber,
                    (*inst_list_it)->isIssued(),
                    (*inst_list_it)->isSquashed());
            inst_list_it++;
            ++num;
        }
    }

    cprintf("Memory dependence hash size: %i\n", memDepHash.size());

#ifdef GEM5_DEBUG
    cprintf("Memory dependence entries: %i\n", MemDepEntry::memdep_count);
#endif
}

} // namespace o3
} // namespace gem5
