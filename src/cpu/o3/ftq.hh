/*
 * Copyright (c) 2022-2023 The University of Edinburgh
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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


#ifndef __CPU_O3_FTQ_HH__
#define __CPU_O3_FTQ_HH__

#include <list>
#include <string>

#include "arch/generic/pcstate.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/limits.hh"
#include "mem/request.hh"
#include "sim/faults.hh"
#include "sim/probe/probe.hh"

#define FDIP
namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;
class FTQ;

struct DerivO3CPUParams;

// Fetch target sequence type. Basically the same as the instruction sequence
// number but since the fetch target is generated before instructions.
typedef InstSeqNum FTSeqNum;


/** The fetch target class. */
class FetchTarget
{
  public:
    FetchTarget(const FTQ& parent, const PCStateBase &start_pc,
                FTSeqNum _seqNum);
    ~FetchTarget();
  private:
    /** The parent ftq */
    const FTQ &ftq;

    /** Start PC and address of the fetch target */
    std::unique_ptr<PCStateBase> startPC;
    Addr startAddr;

    /** End PC of the fetch target */
    std::unique_ptr<PCStateBase> endPC;
    /** The very last address of the fetch target PC + instr. addr*/
    Addr endAddr;

    /** Predicted target address of the fetch target.
     *  Only valid when the ft ends with branch. */
    std::unique_ptr<PCStateBase> predPC;

    /* Fetch targets sequence number */
    const FTSeqNum ftSeqNum;

    /** Whether the exit instruction is a branch */
    bool is_branch;

    /** If the exit branch is taken */
    bool taken;

    /** Virtual and physical address of the
     * cache block comprising the fetch target */
    Addr vaddr;
    Addr paddr;

    /** Translation status */
    bool translation_done;
    bool paddr_valid;

    Tick translation_start = 0;

  public:
    /** Ancore point to attach a branch predictor history.
     * Will carry information while FT is waiting in th FTQ. */
    void* bpu_history;

    /* Start address of the fetch target */
    Addr startAddress() { return startPC->instAddr(); }

    /* PC address of the last instruction in the fetch target */
    Addr endPCAddr() { return (endPC) ? endPC->instAddr() : MaxAddr; }

    /* Address of the last byte in the fetch target */
    Addr endAddress() { return endAddr; }

    /* Fetch Target size (number of bytes) */
    unsigned size() { return endAddress() - startAddress(); }

    bool inRange(Addr addr) {
        return addr >= startAddress() && addr <= endAddress();
    }

    bool inRangeAligned(Addr addr, int alignment) {
        return addr >= (startAddress() & ~(alignment - 1))
            && addr <= (endAddress() & ~(alignment - 1));
    }

    bool isExitInst(Addr addr) {
        return addr == endPCAddr();
    }

    bool isExitBranch(Addr addr) {
        return (addr == endPCAddr()) && is_branch;
    }

    bool hasExceeded(Addr addr) {
        return addr > endAddress();
    }

    Addr getBlkAddr() {
        return vaddr;
    }

    /** Returns the fetch target number. */
    FTSeqNum ftNum() { return ftSeqNum; }

    /** Set the predicted target of the exit branch. */
    void setPredTarg(const PCStateBase &pred_pc) { set(predPC, pred_pc); }

    /** Read the predicted target of the exit branch. */
    const PCStateBase &readPredTarg() { return *predPC; }

    /** Read the start address PC */
    const PCStateBase &readStartPC() { return *startPC; }

    /** Read the exit/end address PC */
    const PCStateBase &readEndPC() { return *endPC; }


    /** Check if the exit branch was predicted taken. */
    bool predTaken() { return taken; }
    bool isFallThrough() { return !taken; }

    /** Complete a fetch target with the exit instruction
     * @param exit_pc The PCState of the exit instruction.
     * @param end_addr The last address in the fetch target (PC + instr.
     *                size).
     * @param sn The fetch target sequence number.
     * @param _is_branch Whether the exit instruction is a branch.
     * @param pred_taken Whether the exit branch was predicted taken.
     * @param pred_pc The predicted target of the exit branch.
    */
    void finalize(const PCStateBase &exit_pc, const Addr end_addr,
                  InstSeqNum sn, bool _is_branch,
                  bool pred_taken, const PCStateBase &pred_pc);



    /** Fetch target status. */
    enum Status
    {
        // Initial state, no translation or prefetching has been issued yet.
        Initial,
        // States to track translation progress
        TranslationInProgress,
        TranslationFailed,
        TranslationReady,
        // State to track prefetching progress
        PrefetchInProgress,
        ReadyToFetch,
        //
        FetchInProgress,
        Invalid
    } state;


    /** Check if the current fetch target was already translated */
    // Addr isTranslated() { return translated; }
    Addr getPaddr() { return paddr; }
    bool hasPaddr() { return paddr_valid; }

    RequestPtr popReq() { return std::move(req); }
    RequestPtr req;
    Fault fault;

    bool initial() { return state == Initial; }
    bool prefetchInProgress() { return state == PrefetchInProgress; }
    bool translationInProgress() { return state == TranslationInProgress; }
    bool translationFailed() { return state == TranslationFailed;}
    bool translationReady() { return state == TranslationReady; }

    bool requiresTranslation() { return state == Initial; }
    void startTranslation(RequestPtr _req) {
      state = TranslationInProgress;
      req = _req;
      translation_start = curTick();
    }

    Tick finishTranslation(const Fault &_fault, const RequestPtr &_req,
                           bool prefetch=false);

    void prefetchIssued() {
      state = PrefetchInProgress;
    }

    void markReady() {
      req = nullptr;
      state = ReadyToFetch;
    }


    bool readyToFetch() { return state == ReadyToFetch; }

    bool isValid() { return state != Invalid; }

    /** Print the fetch target for debugging. */
    std::string print();
};


typedef std::shared_ptr<FetchTarget> FetchTargetPtr;



/**
 * The fetch target queue.
 */
class FTQ
{
  public:
    /** FTQ constructor.
     *  @param _cpu   The cpu object pointer.
     *  @param params The cpu params incl. several FTQ-specific parameters.
     */
    FTQ(CPU *_cpu, const BaseO3CPUParams &params);

    std::string name() const;


  private:

    /** Possible FTQ statuses. */
    enum Status
    {
        Invalid,
        Valid,
        Full,
        Locked
    };

    /** Per-thread FTQ status. */
    Status ftqStatus[MaxThreads];

    /** Pointer to the CPU. */
    CPU *cpu;

  public:
    /** Cache block size */
    const unsigned cacheBlkSize;

  private:
    /** Max number of threads */
    const unsigned numThreads;

    /** Number of fetch targets in the FTQ. (per thread) */
    const unsigned numEntries;

    /** Probe points to attach the FDP perfetcher. */
    ProbePointArg<FetchTargetPtr> *ppFTQInsert;
    ProbePointArg<FetchTargetPtr> *ppFTQRemove;

    /** FTQ List of Fetch targets */
    std::list<FetchTargetPtr> ftq[MaxThreads];



public:

    /** Registers probes. */
    void regProbePoints();

    /** Reset the FTQ state */
    void resetState();


    /** Returns the number of free entries in a specific FTQ paritition. */
    unsigned numFreeEntries(ThreadID tid);

    /** Returns the size of the ftq for a specific partition*/
    unsigned size(ThreadID tid);

    /** Returns if a specific thread's queue is full. */
    bool isFull(ThreadID tid);

    /** Returns if the FTQ is empty. */
    bool isEmpty() const;

    /** Returns if a specific thread's queue is empty. */
    bool isEmpty(ThreadID tid) const;


    /** Invalidates all fetch targets in the FTQ.
     * Requires squash to recover. */
    void invalidate(ThreadID tid);

    /** Returns if the FTQ is in a val;id state and its save to consmume
     * fetch targets. */
    bool isValid(ThreadID tid);

    /** Locks the fetch target queue for a given thread. Locking is different
     * from invalidating in that the head/front fetch targets are still
     * valid and accessible. However, all other FTs are invalid and the
     * FTQ must be squashed to recover. */
    void lock(ThreadID tid);

    /** Check if the FTQ is locked. */
    bool isLocked(ThreadID tid);


    /** Iterates forward over all fetch targets in the FTQ from head/front to
     * tail/back and applies a given function. */
    void forAllForward(ThreadID tid, std::function<void(FetchTargetPtr&)> f);

    /** Iterates backward over all fetch targets in the FTQ from tail/back to
     * head/front and applies a given function. */
    void forAllBackward(ThreadID tid, std::function<void(FetchTargetPtr&)> f);

    /** Helper function to find a certain fetch target in the FTQ.
     * The search condition must be provided as a lambda function.
     * The function will iterate over all fetch targets in the FTQ
     * and return the first fetch target that matches the condition.
     */
    FetchTargetPtr findNext(ThreadID tid,
                            std::function<bool(FetchTargetPtr&)> f);

    /** Helper function to find a certain fetch target in the FTQ.
     * The search condition must be provided as a lambda function.
     * The function will iterate over all fetch targets AFTER the head
     * in the FTQ and return the first fetch target that matches the
     * condition.
     */
    FetchTargetPtr findAfterHead(ThreadID tid,
                                 std::function<bool(FetchTargetPtr&)> f);


    /** Pushes a fetch target into the back/tail of the FTQ.
     *  @param fetchTarget Pointer to the fetch target to be inserted.
     */
    void insert(ThreadID tid, FetchTargetPtr fetchTarget);


    /** Squashes all fetch targets in the FTQ for a specific thread. */
    void squash(ThreadID tid);

    /***/
    void squashSanityCheck(ThreadID tid);

    /** Is the head entry ready for the fetch stage to be consumed. */
    bool isHeadReady(ThreadID tid);

    /** Returns a pointer to the head fetch target of a specific thread within
     *  the FTQ.
     *  @return Pointer to the FetchTarget that is at the head of the FTQ.
     */
    FetchTargetPtr readHead(ThreadID tid);

    FetchTargetPtr readNextHead(ThreadID tid);


    /** Updates the head fetch target once its fully processed
     * In case there is still a branch history attached to the
     * head fetch target the FTQ goes into invalid state.
     * @return Whether or not the update was successful.
    */
    bool updateHead(ThreadID tid);

    /** Searches the FTQ if the translation is required for one of the
     * fetch targets in the FTQ in order to prefetch it into the L1 cache.
     * @param fault The fault object that contains the translation result.
     * @return Whether the translation belongs to FT in the FTQ.
     */
    bool finishTranslation(ThreadID tid, const Fault &fault, RequestPtr &req);



    /** Print the all fetch targets in the FTQ for debugging. */
    void printFTQ(ThreadID tid);


  private:

    struct FTQStats : public statistics::Group
    {
        FTQStats(CPU *cpu, FTQ *ftq);

        statistics::Scalar inserts;
        statistics::Scalar removals;
        statistics::Scalar squashes;
        statistics::Scalar locks;

        statistics::Distribution occupancy;
    } stats;
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_FTQ_HH__
