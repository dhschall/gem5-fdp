
#ifndef __MEM_DDIOBRIDGE_HH__
#define __MEM_DDIOBRIDGE_HH__

#include <deque>
#include <unordered_map>

#include "base/types.hh"
#include "mem/qport.hh"
#include "mem/port.hh"
#include "params/DdioBridge.hh"
#include "sim/stats.hh"
#include "debug/AdaptiveDdioBridge.hh"
#include "sim/sim_object.hh"
#include "sim/clocked_object.hh"

#define PORT_TYPE_FOR_MLC 0
#define PORT_TYPE_FOR_LLC 1
#define PORT_TYPE_FOR_MEM 2

#define CACHE_TYPE_MLC 0
#define CACHE_TYPE_LLC 1
#define CACHE_TYPE_OTHER -1

#define TO_MLC 1
#define TO_LLC 2
#define TO_DRAM 3

namespace gem5
{

    typedef struct
    {
        int cache_type = CACHE_TYPE_OTHER;
        int cache_id = -1;
        float untouched_evict_rate = -1;
        float otf_rate = -1;

        uint64_t num_ddio_blks;
        uint64_t num_ref_ddio_header;
        uint64_t num_ref_ddio_body;

    } OnTheFlyInfo;

    // class DdioBridgeParams;
    class DdioBridge;
    class OnTheFlyResponsePort;

    // class DdioBridge;
    class DdioBridge : public ClockedObject
    {
    private:
        /** Port that handles requests that don't match any of the interfaces.*/
        PortID defaultPortID;

        // Test options
        bool do_not_pass_to_mlc;
        bool snoop_via_memside;
        bool normal_DMA_mode;

        const int allow_mlc = 1;
        const int allow_llc = 2;
        const int allow_dram_only = 4;
        int ddio_option[8];
        bool dynamic;
        bool mlc_share;

        bool send_prefetch_hint;
        bool send_header_only;

    protected:
        // class DdioBridgeRequestPort;
        class DdioBridgeResponsePort : public ResponsePort
        {
        private:
            /** The bridge to which this port belongs. */
            DdioBridge &bridge;
            const AddrRangeList ranges;

        public:
            DdioBridgeResponsePort(const std::string &_name, DdioBridge &_bridge);

        protected:
            virtual bool recvTimingReq(PacketPtr pkt);
            virtual Tick recvAtomic(PacketPtr pkt);
            virtual void recvFunctional(PacketPtr pkt);
            virtual AddrRangeList getAddrRanges() const;

            virtual bool recvTimingSnoopResp(PacketPtr pkt);
            virtual void recvRespRetry();

            void sendRetryReq();
            void sendRetrySnoopResp();

            bool isSnooping() const { return true; }

            /**
             * Called by the owner to send a range change
             */

            /*
            void sendRangeChange() const {
                if (!_RequestPort)
                    fatal("%s cannot sendRangeChange() without master port", name());
                _RequestPort->recvRangeChange();
            }
            */

            /**
             * Tick sendAtomicSnoop(PacketPtr pkt);
            void sendFunctionalSnoop(PacketPtr pkt);
            bool sendTimingResp(PacketPtr pkt);
            void sendTimingSnoopReq(PacketPtr pkt);
            void sendRetryReq();
            void sendRetrySnoopResp();
            bool isSnooping() const { return true; }
            void unbind();
            void bind(RequestPort& master_port);

             */
        };

        class DdioBridgeRequestPort : public RequestPort
        {
        private:
            /** The bridge to which this port belongs. */
            DdioBridge &bridge;
            int port_type = PORT_TYPE_FOR_MLC;

            // ReqPacketQueue reqQueue;

        public:
            DdioBridgeRequestPort(const std::string &_name, DdioBridge &_bridge);
            void setPortType(int type) { port_type = type; }
            int getPortType() { return port_type; }

        protected:
            Tick recvAtomicSnoop(PacketPtr pkt);
            void recvFunctionalSnoop(PacketPtr pkt);
            bool recvTimingResp(PacketPtr pkt);
            void recvTimingSnoopReq(PacketPtr pkt);
            void recvReqRetry();
            virtual void recvRetrySnoopResp();
            bool isSnooping() const;

            /** The master and slave ports of the crossbar */
        };

    public:
        DdioBridgeResponsePort cpusidePort;
        DdioBridgeRequestPort llcsidePort;
        DdioBridgeRequestPort memsidePort;
        std::vector<DdioBridgeRequestPort *> mlcsidePorts;

        // SHIN. Adaptive-DDIO w ADQ support
        std::vector<OnTheFlyResponsePort *> onTheFlyResponsePort;
        OnTheFlyResponsePort *findOtfPort(int mlcid, int type);
        virtual ~DdioBridge();

        /** A function used to return the port associated with this object. */
        Port &getRequestPort(const std::string &if_name,
                             PortID idx = InvalidPortID);
        Port &getResponsePort(const std::string &if_name,
                              PortID idx = InvalidPortID);
        Port &getPort(const std::string &if_name, PortID idx);

        PARAMS(DdioBridge);

        DdioBridge(const Params &p);

        RequestPort &getDestinationRequestPort(PacketPtr pkt);
        ResponsePort &getDestinationResponsePort(PacketPtr pkt);

        // For test options
        bool get_do_not_pass_to_mlc() { return do_not_pass_to_mlc; }
        bool get_snoop_via_memside() { return snoop_via_memside; }
        bool get_normal_DMA_mode() { return normal_DMA_mode; }
        bool canSendToObj(int adp, int dest);

        void sendPrefetchHint(PacketPtr pkt, int mlcid);
    };

    class OnTheFlyResponsePort : public ResponsePort
    {
    private:
        /** The bridge to which this port belongs. */
        DdioBridge &bridge;
        const AddrRangeList ranges;

        // SHIN Adaptive-DDIO w ADQ
        OnTheFlyInfo otfinfo;

        // For find grad
        int window_size;
        int grad_counter;
        // For relative grad
        float prev_grad = 0;
        float prev_otf_rate = 1;

        float local_max_otf = -1; // Send To MLC For init
        float threshhold_gradchange;
        float threshhold_otf;
        float threshhold_abs;

        float relative_increase = 10;

        int cache_type;
        int cache_id;
        bool lock_incrrate = false;
        int boost_cnt = 0;

        int send_status = TO_MLC;
        float dataUsage;

    public:
        OnTheFlyResponsePort(const std::string &_name, DdioBridge &_bridge, const DdioBridgeParams &p);
        double calcAndUpdateGradient(OnTheFlyInfo *newinfo);
        double calcGradChange(OnTheFlyInfo *newinfo);

        // IDIO
        void updateSendStatus();

    protected:
        virtual bool recvTimingReq(PacketPtr pkt);
        virtual Tick recvAtomic(PacketPtr pkt);
        virtual void recvFunctional(PacketPtr pkt);
        virtual AddrRangeList getAddrRanges() const;
        virtual bool recvTimingSnoopResp(PacketPtr pkt);
        virtual void recvRespRetry();

        void sendRetryReq();
        void sendRetrySnoopResp();
        bool isSnooping() const { return true; }

    public:
        void Init(OnTheFlyInfo *info);
        void recvOtfInfo(OnTheFlyInfo *info);
        int getCacheId() { return cache_id; }
        int getCacheType() { return cache_type; }

        bool canSendToCache();

        // SHIN Adaptive-DDIO ADQ
        OnTheFlyInfo *getOnTheFlyInfo() { return &otfinfo; }
    };

}
#endif