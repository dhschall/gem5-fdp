#ifndef __CPU_O3_RECORDER_HH__
#define __CPU_O3_RECORDER_HH__

#include "params/Recorder.hh"
#include "base/types.hh"
#include "base/statistics.hh"
#include "sim/sim_object.hh"
#include "sim/probe/probe.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "mem/request.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "util/recorder_listener.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include <deque>
#include <map>
#include <unordered_set>

namespace gem5
{

class Recorder : public SimObject
{
  public:
    /** Pointer to the CPU. */
    BaseCPU *cpu;
    prefetch::Base *hwp; // replay
    BaseCache *l1icache;
    BaseCache *l2cache;
    void addCaches(BaseCache *_l1icache, BaseCache*_l2cache);

    // size of segment
    int seg_size = 512;

  public:
    RequestorID _requestorId = 0;
    unsigned fetchBufferSize;
    Addr fetchBufferMask;
    Addr fetchBufferAlignPC(Addr addr){ return (addr & ~(fetchBufferMask)); }
    unsigned logBlock;

    // replaying
    std::deque<uint64_t> replaying_timingcomsegs; // seg timing replay

    // listening tid
    uint64_t tid = 0;
    ThreadContext *listening_tc = nullptr;
    int in_roi = 0;

    // metadata
    Addr addr_occupied = 0xbf000000;
    Addr addr_limit = 0xbfffffff;
    typedef struct{
        int maxlen; // sum of rlist.len
        int lastlen; // total length in cacheblocks or addrs
        std::deque<std::pair<Addr, Addr>> rlist; // timing, list of regions
        std::deque<uint64_t> segs; // seg_timing, triggers for segs
        std::deque<int> seglist; // seg_timing, sizes for segs
    } RegionList;
    std::map<uint64_t, RegionList> tid_comregions;

    // compacted trace
    int spatial_pre = 8;
    int spatial_succ = 24;
    struct SpatialRegion
    {
        Addr trigger_blk;
        std::vector<bool> prec;
        std::vector<bool> succ;
        SpatialRegion() {}
        SpatialRegion(Addr, unsigned int, unsigned int);

        bool inSameSpatialRegion(Addr addr_blk, bool update);
        Addr distanceFromTrigger(Addr addr_blk) const;
    };
    int compactor_entries = 16; // entries in temporal_compactor
    std::deque<SpatialRegion> temporal_compactor;

    // compact regions
    RegionList curr_comregionlist;
    std::vector<SpatialRegion> curr_comregion;
    Addr last_pc_blk;
    std::vector<SpatialRegion> curr_comdata; // compacted trace of running task
    std::map<uint64_t, std::vector<SpatialRegion>> tid_comdata;
    int compact_seg_size = 32;
    std::deque<std::vector<SpatialRegion>>                      curr_comdata_vec;
    std::deque<uint64_t>                                        comdata_segs;

    // bundle address table
    struct IndexEntry : public TaggedEntry { uint64_t taskid; };
    AssociativeSet<IndexEntry> index;

    // listening trace
    std::vector<Addr>                               *vaddr_trace = nullptr; // instruction trace from commit
    std::map<uint64_t, std::vector<Addr>*>          tid_ptrace_last; // last
    std::map<uint64_t, std::vector<Addr>>           tid_ptrace_last_unique; // remove duplication, last

    // seg_replay
    std::deque<std::vector<Addr>>                       vtrace_vec = std::deque<std::vector<Addr>>();
    std::deque<std::vector<Addr>>                       ptrace_vec = std::deque<std::vector<Addr>>(); // instruction trace vec by seg-size blocks
    std::deque<uint64_t>                                segs = std::deque<uint64_t>(); // corresponding segs
    std::map<uint64_t, std::deque<std::vector<Addr>>>   tid_ptrace_vec = std::map<uint64_t, std::deque<std::vector<Addr>>>();
    std::map<uint64_t, std::deque<std::vector<Addr>>>   tid_vtrace_vec = std::map<uint64_t, std::deque<std::vector<Addr>>>();
    std::map<uint64_t, std::deque<uint64_t>>            tid_segs;
    
    // statistics
    uint64_t start_tick = 0;
    uint64_t inst_num_committed = 0; // inst committed in current task from task begin

    // options
    bool always_record = true;

    // metadata & mem related
    std::unordered_set<uint64_t> tasks_in_mem; // metadata already in memory
    uint64_t tid_writing = 0; // currently writing tid metadata

    typedef struct{
        Addr addr;
        uint8_t data[8];
        uint64_t isfirst = 0;
        uint64_t islast = 0;
    } WriteUnit; // 8 bytes each
    std::deque<WriteUnit> writing_list; // to be written

    std::vector<ProbeListener *> listeners;

    /** Recorder constructor.
     *  @param _cpu   The cpu object pointer.
     *  @param params The cpu params including several Recorder-specific parameters.
     */
    Recorder(const RecorderParams &params);

    Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;
    void setCpu(BaseCPU* _cpu, RequestorID rid, prefetch::Base *_hwp);
    void regProbeListeners() override; // register probelisteners, called by o3cpu
    std::string name() const;

    EventFunctionWrapper tickEvent;
    void tick();

    // counting misses
    void roi_begin();
    void roi_end();

    struct StatGroup : public statistics::Group
    {
        StatGroup(statistics::Group *parent);
        statistics::Scalar taskNum; // uniq task counts
        statistics::Scalar taskBegins; // task record begin counts
        statistics::Scalar taskEnds; // task record end counts
    } recorderStats;

    void process_bb(Addr vaddr_blk);
    std::map<Addr, std::set<Addr>> curr_bb_succs;
    std::pair<Addr, Addr> curr_bb;
    std::pair<Addr, Addr> last_bb;

    Addr trans(Addr vaddr);
    void startListening(gem5::ThreadContext *tc, uint64_t taskid); // pseudo instructions
    void finishListening(gem5::ThreadContext *tc, uint64_t taskid);
    void listenCommit(const o3::DynInstPtr& a);

    void startRecordingMiss(uint64_t tid) {};
    void finishRecordingMiss(uint64_t tid) {};

    // port
    class IcachePort : public RequestPort // port
    {
      private:
        Recorder *recorder;

      public:
        IcachePort(Recorder *recorder);
        ~IcachePort() {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override{
            recorder->inflight_write--;
            return true;
        };
        void recvReqRetry() { recorder->recvReqRetry(); }; // seems to not be used
    };
    IcachePort icachePort;

    PacketPtr retryPkt = nullptr;
    bool cacheBlocked = false;
    
    int max_inflight_write = 50;
    int inflight_write = 0;

    void tryWrite(Addr paddr);
    void recvReqRetry();
};

} // namespace gem5

#endif //__CPU_O3_RECORDER_HH__
