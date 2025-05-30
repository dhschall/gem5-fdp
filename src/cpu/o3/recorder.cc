#include "cpu/o3/recorder.hh"
#include "cpu/o3/dyn_inst.hh"
#include "params/BaseO3CPU.hh"
#include "debug/CountTask.hh"
#include "mem/cache/prefetch/hp.hh" // cypredar
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "cpu/o3/cpu.hh"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <numeric>

namespace gem5
{

bool sortByValue(const std::pair<uint64_t, uint64_t> a, const std::pair<uint64_t, uint64_t> b) {
    return a.second > b.second;
}

// not realistic, to be improved
std::vector<Addr> remove_duplicates(std::vector<Addr> *origin) {
    assert(origin);
    std::unordered_set<Addr> set;
    std::vector<Addr> no_repeat;
    for(int i = 0; i < origin->size(); i++) {
        if(set.count((*origin)[i]) == 0){
            set.insert((*origin)[i]);
            no_repeat.push_back((*origin)[i]);
        }
    }
    return no_repeat;
}


Recorder::IcachePort::IcachePort(Recorder *_recorder)
    : RequestPort("recorder.iport"), recorder(_recorder)
{}


Recorder::Recorder(const RecorderParams &params):
      SimObject(params),
      fetchBufferSize(params.fetchBufferSize),
      fetchBufferMask(fetchBufferSize - 1),
      logBlock(params.log_block_size),
      index(params.index_assoc, params.index_entries, params.index_indexing_policy,
            params.index_replacement_policy),
      tickEvent([this]{ tick(); }, "recorder tick", false),
      recorderStats(this), icachePort(this)
{
    curr_bb_succs = std::map<Addr, std::set<Addr>>();
    curr_bb = std::make_pair(0, 0);
    last_bb = std::make_pair(0, 0);
    
    curr_comregionlist.lastlen = 0;
    curr_comregionlist.maxlen = 0;
    curr_comregionlist.rlist = std::deque<std::pair<Addr, Addr>>();
    curr_comregionlist.seglist = std::deque<int>();
    curr_comregionlist.segs = std::deque<uint64_t>();
    curr_comregion = std::vector<SpatialRegion>();

    // reset statistics
    in_roi = 1;
    inst_num_committed = 0;
    curr_bb_succs.clear();
    curr_bb = std::make_pair(0, 0);
    last_bb = std::make_pair(0, 0);
    curr_comregionlist.lastlen = 0;
    curr_comregionlist.maxlen = 0;
    curr_comregionlist.rlist.clear();
    curr_comregionlist.seglist.clear();
    curr_comregionlist.segs.clear();
    curr_comregion.clear();
    ptrace_vec.clear();
    vtrace_vec.clear();
    segs.clear();
    curr_comdata.clear();
    curr_comdata_vec.clear();
    comdata_segs.clear();
    temporal_compactor.clear();
}


Recorder::SpatialRegion::SpatialRegion(Addr addr_blk,
    unsigned int prec_size, unsigned int succ_size) {
    trigger_blk = addr_blk;
    prec.resize(prec_size, false);
    succ.resize(succ_size, false);
}
Addr Recorder::SpatialRegion::distanceFromTrigger(Addr target_blk) const
{
    return target_blk > trigger_blk ?
              target_blk - trigger_blk : trigger_blk - target_blk;
}
bool Recorder::SpatialRegion::inSameSpatialRegion(Addr pc_blk, bool update) {
    Addr blk_distance = distanceFromTrigger(pc_blk);
    bool hit = (pc_blk > trigger_blk) ?
        (succ.size() >= blk_distance) : (prec.size() >= blk_distance);
    if (hit && update) {
        if (pc_blk > trigger_blk) {
            succ[blk_distance-1] = true;
        } else if (pc_blk < trigger_blk) {
            prec[blk_distance-1] = true;
        }
    }
    return hit;
}


Recorder::StatGroup::StatGroup(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(taskNum, statistics::units::Count::get(),
        "number of different tasks"),
    ADD_STAT(taskBegins, statistics::units::Count::get(),
        "number of enter startListening"),
    ADD_STAT(taskEnds, statistics::units::Count::get(),
        "number of enter finishListening")
{}

void Recorder::setCpu(BaseCPU* _cpu, RequestorID rid, prefetch::Base *_hwp) {
    cpu = _cpu;
    _requestorId = rid;
    hwp = _hwp;
}

void Recorder::addCaches(BaseCache *_l1icache, BaseCache*_l2cache) {
    DPRINTF(CountTask, "addCaches, %p, %p\n", _l1icache, _l2cache);
    l1icache = _l1icache;
    l2cache = _l2cache;
}

std::string Recorder::name() const{
    return cpu->name() + ".Recorder";
};

void Recorder::regProbeListeners(){
    DPRINTF(CountTask, "recorder register Probe Listeners, L1i%p, L2%p\n", l1icache, l2cache);
    listeners.push_back(
        new RecorderListenerBaseArg<Recorder, o3::DynInstPtr>(
            this,
            cpu->getProbeManager(),
            "Commit",
            & Recorder::listenCommit
        )
    ); // listeners is just container
    assert(l1icache != NULL);
    assert(l2cache != NULL);
}


void Recorder::process_bb(Addr vaddr_blk) {
    if (curr_bb.first == 0 && curr_bb.second == 0) {
        curr_bb.first = vaddr_blk;
        curr_bb.second = vaddr_blk;
        return;
    }
    if (vaddr_blk >= curr_bb.first && vaddr_blk <= curr_bb.second) {
        return;
    }
    if (vaddr_blk == curr_bb.second + 1) {
        curr_bb.second = vaddr_blk;
        return;
    }
    assert(vaddr_blk < curr_bb.first || vaddr_blk > curr_bb.second + 1);
    if (last_bb.first != 0) {
        curr_bb_succs[last_bb.first].insert(vaddr_blk);
    }
    last_bb.first = curr_bb.first;
    last_bb.second = curr_bb.second;
    curr_bb.first = vaddr_blk;
    curr_bb.second = vaddr_blk;
}

void Recorder::listenCommit(const o3::DynInstPtr& a){
    if(!tid) return;

    inst_num_committed++;
    if (cpu->hp) {
        if (replaying_timingcomsegs.size() > 0) {
            if (inst_num_committed >= replaying_timingcomsegs[0]) {
                replaying_timingcomsegs.pop_front();
                prefetch::HierarchicalPrefetcher *fdphp = dynamic_cast<prefetch::HierarchicalPrefetcher *>(hwp);
                assert(fdphp);
                fdphp->pushSegTiming();
            }
        }
    }

    Addr pc = fetchBufferAlignPC(a->pcState().instAddr()); // vaddr

    // compacting vtrace
    Addr vaddr_blk = pc >> logBlock;
    if (vaddr_blk != last_pc_blk) {
        process_bb(vaddr_blk);
        Addr paddr_blk = trans(pc) >> logBlock;
        if (paddr_blk == 0) return;

        bool found = false;
        for (int i = 0; i < temporal_compactor.size(); i++) {
            if (temporal_compactor[i].inSameSpatialRegion(paddr_blk, true)) {
                found = true;
                break;
            }
        }
        if (!found) {
            // comdata_segs
            int sr_in_task = temporal_compactor.size();
            for (int i = 0; i < curr_comdata_vec.size(); i++)
                sr_in_task += curr_comdata_vec[i].size();
            if (sr_in_task % compact_seg_size == 0)
                comdata_segs.push_back(inst_num_committed);

            temporal_compactor.push_back(SpatialRegion(paddr_blk, spatial_pre, spatial_succ));
            if (temporal_compactor.size() > compactor_entries) {
                SpatialRegion newRegion = temporal_compactor.front();
                temporal_compactor.pop_front();
                curr_comdata.push_back(newRegion);

                if (cpu->hp) {
                    if (curr_comdata_vec.size() != 0 && curr_comdata_vec.back().size() == compact_seg_size) { // send write requests
                        std::vector<SpatialRegion> curr_comregion = curr_comdata_vec.back();
                        // write: writing_compact_seg
                        assert(curr_comregion.size() == compact_seg_size);
                        assert(spatial_pre % 8 == 0 && spatial_succ % 8 == 0);

                        // first record
                        if (tid_comregions.count(tid) == 0) { // first record
                            curr_comregionlist.lastlen += curr_comregion.size();
                            curr_comregionlist.maxlen += curr_comregion.size();
                            curr_comregionlist.seglist.push_back(curr_comregion.size());
                            curr_comregionlist.rlist.push_back(std::make_pair(addr_occupied, addr_occupied+curr_comregion.size()*16));
                            
                            // write
                            Addr curr_writing = addr_occupied;
                            for (int i = 0; i < curr_comregion.size(); i++) {
                                // write addr
                                WriteUnit u1;
                                u1.addr = curr_writing;
                                for (int j = 0; j < 8; ++j)
                                    u1.data[j] = (curr_comregion[i].trigger_blk >> j*8) & 0xff;
                                curr_writing += 8;
                                writing_list.push_back(u1);

                                // write prec and succ
                                WriteUnit u2;
                                u2.addr = curr_writing;
                                int data_pos = 0;
                                for (int j = 0; j < curr_comregion[i].prec.size(); j += 8) { // prec
                                    uint8_t byte = 0;
                                    for (int k = 0; k < 8; k++)
                                        byte |= (curr_comregion[i].prec[j+k] << k);
                                    u2.data[data_pos] = byte;
                                    data_pos++;
                                }
                                for (int j = 0; j < curr_comregion[i].succ.size(); j += 8) { // succ
                                    uint8_t byte = 0;
                                    for (int k = 0; k < 8; k++)
                                        byte |= (curr_comregion[i].succ[j+k] << k);
                                    u2.data[data_pos] = byte;
                                    data_pos++;
                                }
                                for (int j = data_pos; j < 8; j++) u2.data[j] = 0;
                                assert(data_pos == spatial_pre/8 + spatial_succ/8);
                                curr_writing += 8;
                                writing_list.push_back(u2);
                            }
                            if (!tickEvent.scheduled())
                                schedule(tickEvent, cpu->clockEdge(Cycles(0))); // schedule event when writing_list.push

                            assert(addr_occupied + curr_comregion.size()*16 == curr_writing);
                            addr_occupied = curr_writing;
                            assert(addr_occupied < addr_limit);
                            if(addr_occupied % fetchBufferSize != 0)
                                addr_occupied = fetchBufferAlignPC(addr_occupied) + fetchBufferSize; // addr_occupied
                        }
                        // record again
                        else {
                            if (curr_comregionlist.maxlen == 0) {
                                curr_comregionlist.maxlen = tid_comregions[tid].maxlen;
                                curr_comregionlist.rlist = tid_comregions[tid].rlist;
                            } // init
                            curr_comregionlist.lastlen += curr_comregion.size();
                            curr_comregionlist.seglist.push_back(curr_comregion.size());

                            if (curr_comregionlist.lastlen > curr_comregionlist.maxlen) { // exceed
                                int new_size = curr_comregionlist.lastlen - curr_comregionlist.maxlen;
                                curr_comregionlist.rlist.push_back(std::make_pair(addr_occupied, addr_occupied+16*new_size));
                                curr_comregionlist.maxlen = curr_comregionlist.lastlen;
                                addr_occupied += new_size * 16;
                                assert(addr_occupied < addr_limit);
                                if(addr_occupied % fetchBufferSize != 0)
                                    addr_occupied = fetchBufferAlignPC(addr_occupied) + fetchBufferSize;
                            }

                            uint64_t distance_to_begin = 16 * (curr_comregionlist.lastlen - curr_comregion.size());
                            Addr curr_writing = curr_comregionlist.rlist[0].first;
                            int writing_comregion = 0;
                            for (int i = 0; i < curr_comregionlist.rlist.size(); i++) {
                                if (distance_to_begin >= curr_comregionlist.rlist[i].second - curr_comregionlist.rlist[i].first) {
                                    assert(curr_comregionlist.rlist.size() > i+1);
                                    curr_writing = curr_comregionlist.rlist[i+1].first;
                                    distance_to_begin -= curr_comregionlist.rlist[i].second - curr_comregionlist.rlist[i].first;
                                    writing_comregion = i+1;
                                } else {
                                    curr_writing += distance_to_begin;
                                    break;
                                }
                            } // find write begin addr: curr_writing

                            for (int i = 0; i < curr_comregion.size(); i++) {
                                WriteUnit u1;
                                u1.addr = curr_writing;
                                for (int j = 0; j < 8; ++j) {
                                    u1.data[j] = (curr_comregion[i].trigger_blk >> j*8) & 0xff;
                                }
                                writing_list.push_back(u1);
                                curr_writing += 8;

                                // write prec and succ
                                WriteUnit u2;
                                u2.addr = curr_writing;
                                int data_pos = 0;
                                for (int j = 0; j < curr_comregion[i].prec.size(); j += 8) { // prec
                                    uint8_t byte = 0;
                                    for (int k = 0; k < 8; k++)
                                        byte |= (curr_comregion[i].prec[j+k] << k);
                                    u2.data[data_pos] = byte;
                                    data_pos++;
                                }
                                for (int j = 0; j < curr_comregion[i].succ.size(); j += 8) { // succ
                                    uint8_t byte = 0;
                                    for (int k = 0; k < 8; k++)
                                        byte |= (curr_comregion[i].succ[j+k] << k);
                                    u2.data[data_pos] = byte;
                                    data_pos++;
                                }
                                for (int j = data_pos; j < 8; j++) u2.data[j] = 0;
                                assert(data_pos == spatial_pre/8 + spatial_succ/8);
                                writing_list.push_back(u2);
                                curr_writing += 8;

                                if (i == curr_comregion.size() - 1) break;

                                assert((curr_comregionlist.rlist[writing_comregion].second - curr_comregionlist.rlist[writing_comregion].first) % 16 == 0);
                                if (curr_writing >= curr_comregionlist.rlist[writing_comregion].second) {
                                    writing_comregion++;
                                    assert(curr_comregionlist.rlist.size() > writing_comregion);
                                    curr_writing = curr_comregionlist.rlist[writing_comregion].first;
                                }
                            }
                            if(!tickEvent.scheduled())
                                schedule(tickEvent, cpu->clockEdge(Cycles(0))); // schedule event when writing_list.push
                        }
                        curr_comregion.clear();
                    }
                }

                // curr_comdata_vec
                if (curr_comdata_vec.size() == 0 || curr_comdata_vec.back().size() == compact_seg_size)
                    curr_comdata_vec.push_back(std::vector<SpatialRegion>());
                curr_comdata_vec.back().push_back(newRegion);
            }
        }
        last_pc_blk = vaddr_blk;
    }

    if (vaddr_trace->size() == 0 || vaddr_trace->back() != pc) { // seg
        // vaddr_trace
        vaddr_trace->push_back(pc);
        Addr vaddr = pc; // v
        Addr paddr = fetchBufferAlignPC(trans(pc));
        if (paddr == 0) return;
        // ptrace_vec & corresponding vtrace_vec for seg_replay
        assert(seg_size > 0);
        if (ptrace_vec.size() == 0 || ptrace_vec.back().size() == seg_size) {
            ptrace_vec.push_back(std::vector<Addr>());
            vtrace_vec.push_back(std::vector<Addr>());
            segs.push_back(inst_num_committed);
        } // add new seg
        if (ptrace_vec.back().size() == 0 || ptrace_vec.back().back() != paddr) {
            ptrace_vec.back().push_back(paddr);
            vtrace_vec.back().push_back(vaddr);
        }
    }
}


Port & Recorder::getPort(const std::string &if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "This object doesn't support vector ports");
    if (if_name == "recorder_port") { return icachePort; }
	else { return SimObject::getPort(if_name, idx); }
}

Addr Recorder::trans(Addr vaddr) { // return 0 if translation failed
    assert(listening_tc);
    // build request
    RequestPtr req = std::make_shared<Request>(
        vaddr, fetchBufferSize,
        Request::PREFETCH, cpu->instRequestorId(), vaddr,
        listening_tc->contextId()); // tid?
    // mmu functional translate
    BaseMMU *mmu = listening_tc->getMMUPtr();
    Fault fault = mmu->translateFunctional(req, listening_tc, BaseMMU::Read);
    if(!req->hasPaddr()){
        return 0;
    }
    assert(cpu->system->isMemAddr(req->getPaddr()));
    return req->getPaddr();
}

void Recorder::startListening(gem5::ThreadContext *tc, uint64_t taskid){
    if(listening_tc != nullptr){
        DPRINTF(CountTask, "warn: already listening; squash listening, start new listen\n");
        // finishListening(tc, 0);
        delete vaddr_trace;
        vaddr_trace = nullptr;
        ptrace_vec.clear();
        vtrace_vec.clear();
        segs.clear();
        curr_comdata.clear();
        curr_comdata_vec.clear();
        comdata_segs.clear();
        temporal_compactor.clear();
        curr_bb_succs.clear();
        curr_bb = std::make_pair(0, 0);
        last_bb = std::make_pair(0, 0);
        curr_comregionlist.lastlen = 0;
        curr_comregionlist.maxlen = 0;
        curr_comregionlist.rlist.clear();
        curr_comregionlist.seglist.clear();
        curr_comregionlist.segs.clear();
        curr_comregion.clear();
        tid = 0;
        listening_tc = nullptr;
        inst_num_committed = 0;
    }
    assert(tid == 0);
    assert(listening_tc == nullptr);
    assert(inst_num_committed == 0);

    startRecordingMiss(taskid); // record cpu0 l2 misses

    tid = taskid;
    listening_tc = tc;
    vaddr_trace = new std::vector<Addr>();
}

void Recorder::finishListening(gem5::ThreadContext *tc, uint64_t taskid){ // ignore taskid
    if(listening_tc == nullptr){ // not listening
        DPRINTF(CountTask, "warn: not listening, do nothing\n");
        return;
    }
    if(listening_tc != tc) DPRINTF(CountTask, "warn: listening_tc != tc\n");

    // must before update tid_ptrace_last_unique
    finishRecordingMiss(tid); // compute coverages

    if(always_record || tid_ptrace_last.count(tid) == 0) { // need record
        tid_ptrace_vec[tid] = ptrace_vec;
        tid_vtrace_vec[tid] = vtrace_vec;
        tid_segs[tid] = segs;

        // compacted data
        while (temporal_compactor.size() > 0) {
            SpatialRegion tmpRegion = temporal_compactor.front();
            temporal_compactor.pop_front();
            curr_comdata.push_back(tmpRegion);

            if (cpu->hp) {
                if (curr_comdata_vec.size() != 0 && curr_comdata_vec.back().size() == compact_seg_size) { // send write requests
                    std::vector<SpatialRegion> curr_comregion = curr_comdata_vec.back();
                    // write: writing_compact_seg
                    assert(curr_comregion.size() == compact_seg_size);
                    assert(spatial_pre % 8 == 0 && spatial_succ % 8 == 0);

                    // first record
                    if (tid_comregions.count(tid) == 0) { // first record
                        curr_comregionlist.lastlen += curr_comregion.size();
                        curr_comregionlist.maxlen += curr_comregion.size();
                        curr_comregionlist.seglist.push_back(curr_comregion.size());
                        curr_comregionlist.rlist.push_back(std::make_pair(addr_occupied, addr_occupied+curr_comregion.size()*16));
                        
                        // write
                        Addr curr_writing = addr_occupied;
                        for (int i = 0; i < curr_comregion.size(); i++) {
                            // write addr
                            WriteUnit u1;
                            u1.addr = curr_writing;
                            for (int j = 0; j < 8; ++j)
                                u1.data[j] = (curr_comregion[i].trigger_blk >> j*8) & 0xff;
                            curr_writing += 8;
                            writing_list.push_back(u1);

                            // write prec and succ
                            WriteUnit u2;
                            u2.addr = curr_writing;
                            int data_pos = 0;
                            for (int j = 0; j < curr_comregion[i].prec.size(); j += 8) { // prec
                                uint8_t byte = 0;
                                for (int k = 0; k < 8; k++)
                                    byte |= (curr_comregion[i].prec[j+k] << k);
                                u2.data[data_pos] = byte;
                                data_pos++;
                            }
                            for (int j = 0; j < curr_comregion[i].succ.size(); j += 8) { // succ
                                uint8_t byte = 0;
                                for (int k = 0; k < 8; k++)
                                    byte |= (curr_comregion[i].succ[j+k] << k);
                                u2.data[data_pos] = byte;
                                data_pos++;
                            }
                            for (int j = data_pos; j < 8; j++) u2.data[j] = 0;
                            assert(data_pos == spatial_pre/8 + spatial_succ/8);
                            curr_writing += 8;
                            writing_list.push_back(u2);
                        }
                        if (!tickEvent.scheduled())
                            schedule(tickEvent, cpu->clockEdge(Cycles(0))); // schedule event when writing_list.push

                        assert(addr_occupied + curr_comregion.size()*16 == curr_writing);
                        addr_occupied = curr_writing;
                        assert(addr_occupied < addr_limit);
                        if(addr_occupied % fetchBufferSize != 0)
                            addr_occupied = fetchBufferAlignPC(addr_occupied) + fetchBufferSize; // addr_occupied
                    }
                    // record again
                    else {
                        if (curr_comregionlist.maxlen == 0) {
                            curr_comregionlist.maxlen = tid_comregions[tid].maxlen;
                            curr_comregionlist.rlist = tid_comregions[tid].rlist;
                        } // init
                        curr_comregionlist.lastlen += curr_comregion.size();
                        curr_comregionlist.seglist.push_back(curr_comregion.size());

                        if (curr_comregionlist.lastlen > curr_comregionlist.maxlen) { // exceed
                            int new_size = curr_comregionlist.lastlen - curr_comregionlist.maxlen;
                            curr_comregionlist.rlist.push_back(std::make_pair(addr_occupied, addr_occupied+16*new_size));
                            curr_comregionlist.maxlen = curr_comregionlist.lastlen;
                            addr_occupied += new_size * 16;
                            assert(addr_occupied < addr_limit);
                            if(addr_occupied % fetchBufferSize != 0)
                                addr_occupied = fetchBufferAlignPC(addr_occupied) + fetchBufferSize;
                        }

                        uint64_t distance_to_begin = 16 * (curr_comregionlist.lastlen - curr_comregion.size());
                        Addr curr_writing = curr_comregionlist.rlist[0].first;
                        int writing_comregion = 0;
                        for (int i = 0; i < curr_comregionlist.rlist.size(); i++) {
                            if (distance_to_begin >= curr_comregionlist.rlist[i].second - curr_comregionlist.rlist[i].first) {
                                assert(curr_comregionlist.rlist.size() > i+1);
                                curr_writing = curr_comregionlist.rlist[i+1].first;
                                distance_to_begin -= curr_comregionlist.rlist[i].second - curr_comregionlist.rlist[i].first;
                                writing_comregion = i+1;
                            } else {
                                curr_writing += distance_to_begin;
                                break;
                            }
                        } // find write begin addr: curr_writing

                        for (int i = 0; i < curr_comregion.size(); i++) {
                            WriteUnit u1;
                            u1.addr = curr_writing;
                            for (int j = 0; j < 8; ++j) {
                                u1.data[j] = (curr_comregion[i].trigger_blk >> j*8) & 0xff;
                            }
                            writing_list.push_back(u1);
                            curr_writing += 8;

                            // write prec and succ
                            WriteUnit u2;
                            u2.addr = curr_writing;
                            int data_pos = 0;
                            for (int j = 0; j < curr_comregion[i].prec.size(); j += 8) { // prec
                                uint8_t byte = 0;
                                for (int k = 0; k < 8; k++)
                                    byte |= (curr_comregion[i].prec[j+k] << k);
                                u2.data[data_pos] = byte;
                                data_pos++;
                            }
                            for (int j = 0; j < curr_comregion[i].succ.size(); j += 8) { // succ
                                uint8_t byte = 0;
                                for (int k = 0; k < 8; k++)
                                    byte |= (curr_comregion[i].succ[j+k] << k);
                                u2.data[data_pos] = byte;
                                data_pos++;
                            }
                            for (int j = data_pos; j < 8; j++) u2.data[j] = 0;
                            assert(data_pos == spatial_pre/8 + spatial_succ/8);
                            writing_list.push_back(u2);
                            curr_writing += 8;

                            if (i == curr_comregion.size() - 1) break;

                            assert((curr_comregionlist.rlist[writing_comregion].second - curr_comregionlist.rlist[writing_comregion].first) % 16 == 0);
                            if (curr_writing >= curr_comregionlist.rlist[writing_comregion].second) {
                                writing_comregion++;
                                assert(curr_comregionlist.rlist.size() > writing_comregion);
                                curr_writing = curr_comregionlist.rlist[writing_comregion].first;
                            }
                        }
                        if(!tickEvent.scheduled())
                            schedule(tickEvent, cpu->clockEdge(Cycles(0))); // schedule event when writing_list.push
                    }
                    curr_comregion.clear();
                }
            }

            if (curr_comdata_vec.size() == 0 || curr_comdata_vec.back().size() == compact_seg_size)
                curr_comdata_vec.push_back(std::vector<SpatialRegion>());
            curr_comdata_vec.back().push_back(tmpRegion);
        } // push entries in compactor into record
        tid_comdata[tid] = curr_comdata;
        assert(curr_comdata_vec.size() == comdata_segs.size());

        // last com seg
        if (cpu->hp) {
            assert(curr_comdata_vec.size() > 0);
            std::vector<SpatialRegion> curr_comregion = curr_comdata_vec.back();
            // write: writing_compact_seg
            assert(curr_comregion.size() <= compact_seg_size && curr_comregion.size() > 0);
            assert(spatial_pre % 8 == 0 && spatial_succ % 8 == 0);
            
            assert(curr_comregionlist.seglist.size() + 1 == comdata_segs.size());

            // first record
            if (tid_comregions.count(tid) == 0) { // first record
                curr_comregionlist.lastlen += curr_comregion.size();
                curr_comregionlist.maxlen += curr_comregion.size();
                curr_comregionlist.seglist.push_back(curr_comregion.size());
                curr_comregionlist.rlist.push_back(std::make_pair(addr_occupied, addr_occupied + curr_comregion.size()*16));
                
                // write
                Addr curr_writing = addr_occupied;
                for (int i = 0; i < curr_comregion.size(); i++) {
                    // write addr
                    WriteUnit u1;
                    u1.addr = curr_writing;
                    for (int j = 0; j < 8; ++j)
                        u1.data[j] = (curr_comregion[i].trigger_blk >> j*8) & 0xff;
                    curr_writing += 8;
                    writing_list.push_back(u1);

                    // write prec and succ
                    WriteUnit u2;
                    u2.addr = curr_writing;
                    int data_pos = 0;
                    for (int j = 0; j < curr_comregion[i].prec.size(); j += 8) { // prec
                        uint8_t byte = 0;
                        for (int k = 0; k < 8; k++)
                            byte |= (curr_comregion[i].prec[j+k] << k);
                        u2.data[data_pos] = byte;
                        data_pos++;
                    }
                    for (int j = 0; j < curr_comregion[i].succ.size(); j += 8) { // succ
                        uint8_t byte = 0;
                        for (int k = 0; k < 8; k++)
                            byte |= (curr_comregion[i].succ[j+k] << k);
                        u2.data[data_pos] = byte;
                        data_pos++;
                    }
                    assert(data_pos == spatial_pre/8 + spatial_succ/8);
                    for (int j = data_pos; j < 8; j++) u2.data[j] = 0;
                    if (i == curr_comregion.size() - 1)
                        u2.islast = tid; // is last writing unit
                    curr_writing += 8;
                    writing_list.push_back(u2);
                }
                if (!tickEvent.scheduled())
                    schedule(tickEvent, cpu->clockEdge(Cycles(0))); // schedule event when writing_list.push
                
                assert(addr_occupied + curr_comregion.size()*16 == curr_writing);
                addr_occupied = curr_writing;
                assert(addr_occupied < addr_limit);
                if(addr_occupied % fetchBufferSize != 0)
                    addr_occupied = fetchBufferAlignPC(addr_occupied) + fetchBufferSize; // addr_occupied
                
                RegionList tmp_comregion;
                tmp_comregion.lastlen = curr_comregionlist.lastlen;
                tmp_comregion.maxlen = curr_comregionlist.maxlen;
                tmp_comregion.rlist = curr_comregionlist.rlist;
                tmp_comregion.seglist = curr_comregionlist.seglist;
                tmp_comregion.segs = comdata_segs;
                tid_comregions[tid] = tmp_comregion;
            }
            // record again
            else {
                assert(tid_comregions.count(tid));
                if (curr_comregionlist.maxlen == 0) {
                    curr_comregionlist.maxlen = tid_comregions[tid].maxlen;
                    curr_comregionlist.rlist = tid_comregions[tid].rlist;
                } // init
                curr_comregionlist.lastlen += curr_comregion.size();
                curr_comregionlist.seglist.push_back(curr_comregion.size());

                if (curr_comregionlist.lastlen > curr_comregionlist.maxlen) { // exceed
                    int new_size = curr_comregionlist.lastlen - curr_comregionlist.maxlen;
                    curr_comregionlist.rlist.push_back(std::make_pair(addr_occupied, addr_occupied+16*new_size));
                    curr_comregionlist.maxlen = curr_comregionlist.lastlen;
                    addr_occupied += new_size * 16;
                    assert(addr_occupied < addr_limit);
                    if(addr_occupied % fetchBufferSize != 0)
                        addr_occupied = fetchBufferAlignPC(addr_occupied) + fetchBufferSize;
                }

                // find write position
                uint64_t distance_to_begin = 16 * (curr_comregionlist.lastlen - curr_comregion.size());
                Addr curr_writing = curr_comregionlist.rlist[0].first;
                int writing_comregion = 0;
                for (int i = 0; i < curr_comregionlist.rlist.size(); i++) {
                    if (distance_to_begin >= curr_comregionlist.rlist[i].second - curr_comregionlist.rlist[i].first) {
                        assert(curr_comregionlist.rlist.size() > i+1);
                        curr_writing = curr_comregionlist.rlist[i+1].first;
                        distance_to_begin -= curr_comregionlist.rlist[i].second - curr_comregionlist.rlist[i].first;
                        writing_comregion = i+1;
                    } else {
                        curr_writing += distance_to_begin;
                        break;
                    }
                } // find write begin addr: curr_writing

                for (int i = 0; i < curr_comregion.size(); i++) {
                    WriteUnit u1;
                    u1.addr = curr_writing;
                    for (int j = 0; j < 8; ++j)
                        u1.data[j] = (curr_comregion[i].trigger_blk >> j*8) & 0xff;
                    writing_list.push_back(u1);
                    curr_writing += 8;

                    // write prec and succ
                    WriteUnit u2;
                    u2.addr = curr_writing;
                    int data_pos = 0;
                    for (int j = 0; j < curr_comregion[i].prec.size(); j += 8) { // prec
                        uint8_t byte = 0;
                        for (int k = 0; k < 8; k++)
                            byte |= (curr_comregion[i].prec[j+k] << k);
                        u2.data[data_pos] = byte;
                        data_pos++;
                    }
                    for (int j = 0; j < curr_comregion[i].succ.size(); j += 8) { // succ
                        uint8_t byte = 0;
                        for (int k = 0; k < 8; k++)
                            byte |= (curr_comregion[i].succ[j+k] << k);
                        u2.data[data_pos] = byte;
                        data_pos++;
                    }
                    for (int j = data_pos; j < 8; j++) u2.data[j] = 0; // 剩余清0
                    assert(data_pos == spatial_pre/8 + spatial_succ/8);
                    if (i == curr_comregion.size() - 1)
                        u2.islast = tid; // is last
                    writing_list.push_back(u2);
                    curr_writing += 8;

                    if (i == curr_comregion.size() - 1) break;

                    assert((curr_comregionlist.rlist[writing_comregion].second - curr_comregionlist.rlist[writing_comregion].first) % 16 == 0);
                    if (curr_writing >= curr_comregionlist.rlist[writing_comregion].second) {
                        writing_comregion++;
                        assert(curr_comregionlist.rlist.size() > writing_comregion);
                        curr_writing = curr_comregionlist.rlist[writing_comregion].first;
                    }
                }
                if(!tickEvent.scheduled())
                    schedule(tickEvent, cpu->clockEdge(Cycles(0))); // schedule event when writing_list.push
                
                tid_comregions[tid].lastlen = curr_comregionlist.lastlen;
                tid_comregions[tid].maxlen = curr_comregionlist.maxlen;
                tid_comregions[tid].rlist = curr_comregionlist.rlist;
                tid_comregions[tid].seglist = curr_comregionlist.seglist;
                tid_comregions[tid].segs = comdata_segs;
            }
            curr_comregion.clear();
        }

        // insert new tid to table
        IndexEntry *idx_entry = index.findEntry(tid, false);
        if (idx_entry != nullptr) { // found
            index.accessEntry(idx_entry);
        } else { // not found
            idx_entry = index.findVictim(tid);
            assert(idx_entry != nullptr);
            index.insertEntry(tid, false, idx_entry);
        }
        idx_entry->taskid = tid;

        // ptrace_aligned
        std::vector<Addr> *ptrace_aligned = new std::vector<Addr>();
        for(int i = 0; i < vaddr_trace->size(); i++){ // translate into paddr
            Addr paddr = trans((*vaddr_trace)[i]);
            if (paddr != 0) ptrace_aligned->push_back(paddr);
        }

        // push ptrace_aligned into tid_ptrace_last
        if (tid_ptrace_last.count(tid) == 0) { // first execute task tid
            tid_ptrace_last[tid] = ptrace_aligned; // ptrace
            std::vector<Addr> ptrace_aligned_unique1 = remove_duplicates(ptrace_aligned);
            tid_ptrace_last_unique[tid] = ptrace_aligned_unique1; // last unique trace

            DPRINTF(CountTask, "task[%#lx] first record succ, len=%d\n", tid, ptrace_aligned->size()); // print
        } else if (always_record) { // record again
            assert(tid_ptrace_last.count(tid) > 0); // already in

            delete tid_ptrace_last[tid];
            tid_ptrace_last[tid] = ptrace_aligned; // ptrace

            std::vector<Addr> ptrace_aligned_unique1 = remove_duplicates(ptrace_aligned);
            tid_ptrace_last_unique[tid] = ptrace_aligned_unique1; // last unique trace
        }
    }
    tasks_in_mem.insert(tid);
    
    // save and clear
    inst_num_committed = 0;
    delete vaddr_trace;
    vaddr_trace = nullptr;
    ptrace_vec.clear();
    vtrace_vec.clear();
    segs.clear();
    curr_comdata.clear();
    curr_comdata_vec.clear();
    comdata_segs.clear();
    temporal_compactor.clear();
    curr_bb_succs.clear();
    curr_bb = std::make_pair(0, 0);
    last_bb = std::make_pair(0, 0);
    curr_comregionlist.lastlen = 0;
    curr_comregionlist.maxlen = 0;
    curr_comregionlist.rlist.clear();
    curr_comregionlist.seglist.clear();
    curr_comregionlist.segs.clear();
    curr_comregion.clear();
    tid = 0;
    listening_tc = nullptr;
}

void Recorder::tick(){
    if (inflight_write < max_inflight_write)
        tryWrite(0);

    if(!tickEvent.scheduled() && !writing_list.empty())
        schedule(tickEvent, cpu->clockEdge(Cycles(1)));
}

void Recorder::roi_begin() {
    printf("roi begin\n");
    DPRINTF(CountTask, "roi_begin, in_roi=%d\n", in_roi);
    in_roi = 1;
    // reset statistics
    inst_num_committed = 0;
    curr_bb_succs.clear();
    curr_bb = std::make_pair(0, 0);
    last_bb = std::make_pair(0, 0);
    curr_comregionlist.lastlen = 0;
    curr_comregionlist.maxlen = 0;
    curr_comregionlist.rlist.clear();
    curr_comregionlist.seglist.clear();
    curr_comregionlist.segs.clear();
    curr_comregion.clear();
    ptrace_vec.clear();
    vtrace_vec.clear();
    segs.clear();
    curr_comdata.clear();
    curr_comdata_vec.clear();
    comdata_segs.clear();
    temporal_compactor.clear();
}

void Recorder::roi_end() {
    DPRINTF(CountTask, "roi_end, in_roi=%d\n", in_roi);
    in_roi = 0;

    // addr_occupied
    DPRINTF(CountTask, "addr_occupied:%#lx, addr_limit:%#lx\n", addr_occupied, addr_limit);
}


void Recorder::tryWrite(Addr paddr){
    if(cacheBlocked)
        return;
    if(writing_list.empty())
        return;

    // writing_list not empty
    int continous = 1;
    for(int i = 1; i < 8 && i < writing_list.size(); i++) {
        if(writing_list[i].addr == writing_list[i-1].addr + 8)
            continous++;
        else break;
    }

    // fill meta_data
    Addr meta_addr = writing_list[0].addr;
    int meta_size = continous * 8;
    uint8_t *meta_data = new uint8_t[meta_size];
    for(int i = 0; i < continous; i++)
        for(int j = 0; j < 8; j++)
            meta_data[i*8+j] = writing_list[i].data[j];

    // build request
    RequestPtr req = std::make_shared<Request>(
        meta_addr, meta_size,
        Request::PHYSICAL, cpu->dataRequestorId());

    assert(req->hasPaddr());
    assert(cpu->system->isMemAddr(req->getPaddr()));

    // build packet
    PacketPtr data_pkt = new Packet(req, MemCmd::WriteReq);
    data_pkt->dataDynamic(meta_data);
    if(!icachePort.sendTimingReq(data_pkt)){ // blocked
        assert(retryPkt == nullptr);
        retryPkt = data_pkt;
        cacheBlocked = true;
    } else{ // sent successed
        inflight_write++;
        for(int i = 0; i < continous; i++)
            writing_list.pop_front();
    }
}


void Recorder::recvReqRetry(){
    if (icachePort.sendTimingReq(retryPkt)) {
        inflight_write++;
        retryPkt = nullptr;
        cacheBlocked = false;
    }
}


} // namespace gem5
