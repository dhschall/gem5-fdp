#include "sim/system.hh"

#include "mem/ddio_bridge.hh"

#include "debug/AdaptiveDdioOtf.hh"
#include "debug/AdaptiveDdioOtfInfo.hh"
#include "debug/AdaptiveDdioBridgeHint.hh"

namespace gem5
{
    Port &
    DdioBridge::getRequestPort(const std::string &if_name, PortID idx)
    {
        if (if_name == "mlcside" && idx < mlcsidePorts.size())
        {
            // the master port index translates directly to the vector position
            return *mlcsidePorts[idx];
        }
        else if (if_name == "default")
        {
            return *mlcsidePorts[defaultPortID];
        }
        else if (if_name == "llcside")
        {
            return llcsidePort;
        }
        else if (if_name == "memside")
        {
            return memsidePort;
        }
        else
        {
            return ClockedObject::getPort(if_name, idx);
        }
    }

    Port &
    DdioBridge::getResponsePort(const std::string &if_name, PortID idx)
    {
        if (if_name == "cpuside")
        {
            return cpusidePort;
        }
        else if (if_name == "otfport")
        {
            return *onTheFlyResponsePort[idx];
        }
        else
            // pass it along to our super class
            return ClockedObject::getPort(if_name, idx);
    }

    Port &
    DdioBridge::getPort(const std::string &if_name, PortID idx)
    {
        if (if_name == "mlcside" && idx < mlcsidePorts.size())
        {
            // the master port index translates directly to the vector position
            return *mlcsidePorts[idx];
        }
        else if (if_name == "default")
        {
            return *mlcsidePorts[defaultPortID];
        }
        else if (if_name == "llcside")
        {
            return llcsidePort;
        }
        else if (if_name == "memside")
        {
            return memsidePort;
        }
        else if (if_name == "cpuside")
        {
            return cpusidePort;
        }
        else if (if_name == "otfport")
        {
            return *onTheFlyResponsePort[idx];
        }
        else
            // pass it along to our super class
            return ClockedObject::getPort(if_name, idx);
    }

    DdioBridge::DdioBridgeResponsePort::DdioBridgeResponsePort(const std::string &_name, DdioBridge &_bridge)
        : ResponsePort(_name, &_bridge), bridge(_bridge)
    {
    }

    bool
    DdioBridge::DdioBridgeResponsePort::recvTimingReq(PacketPtr pkt)
    {
        if (pkt->cmd == MemCmd::WritebackDirty)
            DPRINTF(AdaptiveDdioBridge, "recvTimingReq, pkt %s\n", pkt->print());
        return bridge.getDestinationRequestPort(pkt).sendTimingReq(pkt);
    }

    Tick
    DdioBridge::DdioBridgeResponsePort::recvAtomic(PacketPtr pkt)
    {
        // DPRINTF(AdaptiveDdioBridge, "recvAtomic, pkt %s\n", pkt->print());
        return bridge.getDestinationRequestPort(pkt).sendAtomic(pkt);
    }

    void
    DdioBridge::DdioBridgeResponsePort::recvFunctional(PacketPtr pkt)
    {
        // DPRINTF(AdaptiveDdioBridge, "recvFunctional, pkt %s\n", pkt->print());
        return bridge.getDestinationRequestPort(pkt).sendFunctional(pkt);
        // bridge.llcsidePort.sendFunctional(pkt);
    }

    AddrRangeList
    DdioBridge::DdioBridgeResponsePort::getAddrRanges() const
    {
        // DPRINTF(AdaptiveDdioBridge, "getAddrRanges???\n");
        return ranges;
    }

    bool
    DdioBridge::DdioBridgeResponsePort::recvTimingSnoopResp(PacketPtr pkt)
    {
        // DPRINTF(AdaptiveDdioBridge, "recvTimingSnoopResp, pkt %s\n", pkt->print());
        return bridge.getDestinationRequestPort(pkt).sendTimingSnoopResp(pkt);
    }

    void
    DdioBridge::DdioBridgeResponsePort::recvRespRetry()
    {
        DPRINTF(AdaptiveDdioBridge, "recvRespRetry!\n");
        panic("DdioBridge::DdioBridgeResponsePort::recvRespRetry()");
        // bridge.getDestinationRequestPort(nullptr).sendRetryResp();
    }

    void
    DdioBridge::DdioBridgeResponsePort::sendRetryReq()
    {
        DPRINTF(AdaptiveDdioBridge, "sendRetryReq!\n");
        panic("DdioBridge::DdioBridgeResponsePort::sendRetryReq()");
    }

    void
    DdioBridge::DdioBridgeResponsePort::sendRetrySnoopResp()
    {
        DPRINTF(AdaptiveDdioBridge, "sendRetrySnoopResp!\n");
        panic("DdioBridge::DdioBridgeResponsePort::sendRetrySnoopResp()");
    }

    DdioBridge::DdioBridgeRequestPort::DdioBridgeRequestPort(const std::string &_name, DdioBridge &_bridge)
        : RequestPort(_name, &_bridge), bridge(_bridge)
    {
    }

    Tick
    DdioBridge::DdioBridgeRequestPort::recvAtomicSnoop(PacketPtr pkt)
    {
        if (isSnooping())
        {
            // DPRINTF(AdaptiveDdioBridge, "recvAtomicSnoop, pkt %s\n", pkt->print());
            return bridge.getDestinationResponsePort(pkt).sendAtomicSnoop(pkt);
        }
        panic("recvAtomicSnoop! port typ %s, pkt %s\n", getPortType() == PORT_TYPE_FOR_MLC ? "MLC" : "MEM", pkt->print());
        return 0;
    }

    void
    DdioBridge::DdioBridgeRequestPort::recvFunctionalSnoop(PacketPtr pkt)
    {
        if (1)
        {
            // DPRINTF(AdaptiveDdioBridge, "recvFunctionalSnoop, pkt %s\n", pkt->print());
            bridge.getDestinationResponsePort(pkt).sendFunctionalSnoop(pkt);
            return;
        }
    }

    bool
    DdioBridge::DdioBridgeRequestPort::recvTimingResp(PacketPtr pkt)
    {
        if (isSnooping())
        {
            // DPRINTF(AdaptiveDdioBridge, "recvTimingResp, pkt %s\n", pkt->print());
            return bridge.getDestinationResponsePort(pkt).sendTimingResp(pkt);
        }
        panic("recvTimingResp! port typ %s, pkt %s\n", getPortType() == PORT_TYPE_FOR_MLC ? "MLC" : "MEM", pkt->print());
        return true;
    }

    void
    DdioBridge::DdioBridgeRequestPort::recvTimingSnoopReq(PacketPtr pkt)
    {
        // DPRINTF(AdaptiveDdioBridge, "recvTimingSnoopReq, pkt %s, dest %d\n", pkt->print(), pkt->getDdioPrefetchDestination());

        // DPRINTF(AdaptiveDdioBridge, "recvTimingSnoopReq is blkio %d, mlc %d, hasdata %d, pkt %s\n", pkt->isBlockIO(), pkt->getDdioPrefetchDestination(), pkt->hasData(), pkt->print());
        if ((!(pkt->hasData())) && pkt->cmd == MemCmd::UpgradeReq)
        {
            // DPRINTF(AdaptiveDdioBridge, "recvTimingSnoopReq no data upgrade, pkt %s\n", pkt->print());

            // return;
        }
        return bridge.getDestinationResponsePort(pkt).sendTimingSnoopReq(pkt);
    }

    void
    DdioBridge::DdioBridgeRequestPort::recvReqRetry()
    {
        DPRINTF(AdaptiveDdioBridge, "recvReqRetry\n");
        if (bridge.send_prefetch_hint && port_type == PORT_TYPE_FOR_MLC)
            return;
        bridge.getDestinationResponsePort(nullptr).sendRetryReq();
        return;
    }

    void
    DdioBridge::DdioBridgeRequestPort::recvRetrySnoopResp()
    {

        DPRINTF(AdaptiveDdioBridge, "recvRetrySnoopResp\n");
        bridge.getDestinationResponsePort(nullptr).sendRetrySnoopResp();
    }

    bool
    DdioBridge::DdioBridgeRequestPort::isSnooping() const
    {
        if (bridge.get_snoop_via_memside() || bridge.get_normal_DMA_mode())
            return port_type == PORT_TYPE_FOR_MEM;
        else
            return port_type == PORT_TYPE_FOR_LLC;
    }

    DdioBridge::DdioBridge(const DdioBridgeParams &p)
        : ClockedObject(p),
          do_not_pass_to_mlc(p.do_not_pass_to_mlc),
          snoop_via_memside(p.snoop_via_memside),
          normal_DMA_mode(p.normal_DMA_mode),
          cpusidePort(p.name + ".cpuside", *this),
          llcsidePort(p.name + ".llcside", *this),
          memsidePort(p.name + ".memside", *this)
    {
        for (int i = 0; i < p.port_mlcside_connection_count; ++i)
        {
            DdioBridgeRequestPort *memp = new DdioBridgeRequestPort(p.name + ".mlcside", *this);
            mlcsidePorts.push_back(memp);
            OnTheFlyResponsePort *otfp = new OnTheFlyResponsePort(p.name + ".otfport", *this, p);
            onTheFlyResponsePort.push_back(otfp);
        }
        OnTheFlyResponsePort *otfp = new OnTheFlyResponsePort(p.name + ".otfport", *this, p);
        onTheFlyResponsePort.push_back(otfp);

        llcsidePort.setPortType(PORT_TYPE_FOR_LLC);
        memsidePort.setPortType(PORT_TYPE_FOR_MEM);

        ddio_option[0] = p.ddio_option_app0;
        ddio_option[1] = p.ddio_option_app1;
        ddio_option[2] = p.ddio_option_app2;
        ddio_option[3] = p.ddio_option_app3;

        dynamic = p.dynamic_ddio;
        mlc_share = p.mlc_share;
        send_prefetch_hint = p.send_prefetch_hint;
        send_header_only = p.send_header_only;
    }

    DdioBridge::~DdioBridge()
    {
        for (auto m : mlcsidePorts)
            delete m;
    }

    // DdioBridge *
    // DdioBridgeParams::create() const
    // {
    //     return new DdioBridge(*this);
    // }

    /**
     * It decides which memory object to send the packet to.
     *
     * For now, I did not have a policy, so I made it temporary.
     *
     * 1.  Send pkt to memside only if bypass option is set.
     * 2.  If it can send pkt to MLC. (If there is any MLC that has target blk)
     * 3.  If not, send pkt to LLC.
     *
     * SHIN.
     */
    RequestPort &
    DdioBridge::getDestinationRequestPort(PacketPtr pkt)
    {

        int mlc_idx = pkt->getDdioPrefetchDestination();
        // For Test
        // mlc_idx = 0;
        //    if(mlc_share) mlc_idx /= 2;

        // if(mlc_idx != -1)
        //     DPRINTF(AdaptiveDdioBridge, "On MLC %d, pkt %s\n", mlc_idx, pkt->print());

        if (do_not_pass_to_mlc)
        {
            DPRINTF(AdaptiveDdioBridge, "LLC ddio mode. mlc %d, pkt %s\n", mlc_idx, pkt->print());
            return llcsidePort;
        }
        // if(1)
        // {
        //     if(send_header_only){
        //         if(pkt->isDdioHeader()){
        //             DPRINTF(AdaptiveDdioBridgeHint, "Send Hint Only MLC%d; %s, time %lld\n", mlc_idx, pkt->print(),  curTick() / sim_clock::as_int::ns);
        //             pkt->setPrefetchHintPkt();
        //         }
        //         else{
        //             pkt->unsetPrefetchHintPkt();
        //         }
        //     }
        //     else{
        //         DPRINTF(AdaptiveDdioBridgeHint, "Send All MLC%d; %s, time %lld\n", mlc_idx, pkt->print(),  curTick() / sim_clock::as_int::ns);
        //         pkt->setPrefetchHintPkt();
        //     }
        // }
        // // If it is writback packet
        // if(pkt->cmd==MemCmd::WritebackDirty){
        if (1)
        {
            // if(mlc_idx != -1 && canSendToObj(pkt->getDdioPrefetchDestination(), allow_mlc)){

            // if(mlc_idx != -1 && canSendToObj(mlc_idx, allow_mlc)){
            if (mlc_idx > -1)
            { // For Test
                if (1)
                {
                    if (send_prefetch_hint)
                    {
                        DPRINTF(AdaptiveDdioBridge, "Send Header only MLC%d; pkt %s, time %lld\n", mlc_idx, pkt->print(), curTick() / sim_clock::as_int::ns);
                        DPRINTF(AdaptiveDdioBridge, "Send Hint To MLC%d; pkt %s, time %lld\n", mlc_idx, pkt->print(), curTick() / sim_clock::as_int::ns);
                        DPRINTF(AdaptiveDdioBridgeHint, "Set Hint To MLC%d; pkt %s, time %lld\n", mlc_idx, pkt->print(), curTick() / sim_clock::as_int::ns);
                        //     if(send_header_only){
                        if (send_header_only)
                        {
                            if (pkt->isDdioHeader())
                            {
                                DPRINTF(AdaptiveDdioBridgeHint, "Send Hint Only MLC%d; %s, time %lld\n", mlc_idx, pkt->print(), curTick() / sim_clock::as_int::ns);
                                pkt->setPrefetchHintPkt();
                            }
                            if (send_header_only && !(pkt->isDdioHeader()))
                            {
                                DPRINTF(AdaptiveDdioBridgeHint, "Not Header!! Do not Prefetch! MLC%d; %s, time %lld\n", mlc_idx, pkt->print(), curTick() / sim_clock::as_int::ns);
                                pkt->unsetPrefetchHintPkt();
                            }
                        }
                        else
                        {
                            DPRINTF(AdaptiveDdioBridgeHint, "Send all MLC%d; %s, time %lld\n", mlc_idx, pkt->print(), curTick() / sim_clock::as_int::ns);
                            pkt->setPrefetchHintPkt();
                        }
                    }
                    else
                    {
                        DPRINTF(AdaptiveDdioBridgeHint, "What??? MLC%d; %s, time %lld\n", mlc_idx, pkt->print(), curTick() / sim_clock::as_int::ns);
                        // pkt->setPrefetchHintPkt();
                    }

                    // sendPrefetchHint(pkt, mlc_idx);
                    return llcsidePort;
                }
                // else{
                //     DPRINTF(AdaptiveDdioBridge, "WriteBack To MLC%d; pkt %s, time %lld\n", mlc_idx, pkt->print(),  curTick() / sim_clock::as_int::ns);
                //     return *mlcsidePorts[mlc_idx];
                // }
                // return llcsidePort;
            }

            // Else if can send to LLC
            // if(pkt->isBypassMlc()){
            // if(canSendToObj(mlc_idx, allow_llc)){
            // if(1){      // SHIN. For Test
            //     //OnTheFlyResponsePort* otfport = findOtfPort(-1, CACHE_TYPE_LLC);
            //     if(!dynamic)
            //     {
            //         DPRINTF(AdaptiveDdioBridge, "WriteBack To LLC%d; (Bypass MLC), pkt %s, time %lld\n", pkt->getDdioPrefetchDestination(), pkt->print(), curTick() / sim_clock::as_int::ns);
            //         return llcsidePort;
            //     }
            //     DPRINTF(AdaptiveDdioBridge, "WriteBack To Mem%d; (LLC is full), pkt %s, time %lld\n", pkt->getDdioPrefetchDestination(), pkt->print(), curTick() / sim_clock::as_int::ns);
            //     return memsidePort;
            // }
            // Else send to Mem
            // DPRINTF(AdaptiveDdioBridge, "WriteBack To Mem(There is no blk on Cache), pkt %s\n", pkt->print());
            // return memsidePort;
            // DPRINTF(AdaptiveDdioBridge, "WriteBack To Mem%d;(Unknown), pkt %s, time %lld\n", pkt->getDdioPrefetchDestination(), pkt->print(), curTick() / sim_clock::as_int::ns);
            // return memsidePort;
        }
        // Other Packets
        // not DMA mode
        else
        {
            // DPRINTF(AdaptiveDdioBridge, "WriteBack To LLC, pkt %s, time %lld\n", pkt->print(), curTick() / sim_clock::as_int::ns);
            return llcsidePort;
        }
        return llcsidePort;
    }

    // SHIN. Prefetch
    void
    DdioBridge::sendPrefetchHint(PacketPtr pkt, int mlcid)
    {
        if (!pkt)
            DPRINTF(AdaptiveDdioBridge, "pkt is null\n");
        else if (!(pkt->req))
            DPRINTF(AdaptiveDdioBridge, "pkt->req is null\n");

        PacketPtr hint_pkt = new Packet(pkt, 0, 1);
        DPRINTF(AdaptiveDdioBridge, "build hint_pkt\n");

        // hint_pkt->cmd = MemCmd::ReadReq;
        hint_pkt->setPrefetchHintPkt();
        DPRINTF(AdaptiveDdioBridge, "set PrefetchHintPkt\n");

        DPRINTF(AdaptiveDdioBridge, "Try Send Hint to MLC %d, pkt %s\n", mlcid, hint_pkt->print());

        if (mlcid != -1)
        {
            DPRINTF(AdaptiveDdioBridge, "Send Hint to MLC %d, pkt %s\n", mlcid, hint_pkt->print());
            DPRINTF(AdaptiveDdioBridge, "Addr MLC port %#x\n", mlcsidePorts[mlcid]);
            DPRINTF(AdaptiveDdioBridge, "Addr pair of MLC port %#x\n", mlcsidePorts[mlcid]->getResponsePort());
            mlcsidePorts[mlcid]->sendTimingReq(hint_pkt);
        }
        else
        {
            DPRINTF(AdaptiveDdioBridge, "MLC id is -1..., pkt %s\n", hint_pkt->print());
        }
    }

    ResponsePort &
    DdioBridge::getDestinationResponsePort(PacketPtr pkt)
    {
        return cpusidePort;
    }

    OnTheFlyResponsePort::OnTheFlyResponsePort(const std::string &_name, DdioBridge &_bridge, const DdioBridgeParams &p)
        : ResponsePort(_name, &_bridge), bridge(_bridge), otfinfo()
    {
        prev_grad = 1;
        threshhold_otf = p.threshhold_otf;
        threshhold_gradchange = p.threshhold_gradchange;
        window_size = p.window_size;
        grad_counter = 0;
        local_max_otf = -1;
        threshhold_abs = p.threshhold_abs;
    }

    bool
    OnTheFlyResponsePort::recvTimingReq(PacketPtr pkt)
    {
        return true;
    }

    Tick
    OnTheFlyResponsePort::recvAtomic(PacketPtr pkt)
    {
        return 0;
    }

    void
    OnTheFlyResponsePort::recvFunctional(PacketPtr pkt)
    {
    }

    AddrRangeList
    OnTheFlyResponsePort::getAddrRanges() const
    {
        return ranges;
    }

    bool
    OnTheFlyResponsePort::recvTimingSnoopResp(PacketPtr pkt)
    {
        return true;
    }

    void
    OnTheFlyResponsePort::recvRespRetry()
    {
    }

    void
    OnTheFlyResponsePort::sendRetryReq()
    {
    }

    void
    OnTheFlyResponsePort::sendRetrySnoopResp()
    {
    }

    double
    OnTheFlyResponsePort::calcAndUpdateGradient(OnTheFlyInfo *newinfo)
    {
        double grad = (newinfo->otf_rate - prev_otf_rate) / window_size;
        prev_otf_rate = newinfo->otf_rate;
        if (grad == 0)
        {
            if (grad < 0)
                prev_grad = -0.000001;
            else
                prev_grad = 0.000001;
        }
        prev_grad = grad;

        return grad;
    }

    double
    OnTheFlyResponsePort::calcGradChange(OnTheFlyInfo *newinfo)
    {
        double prev_grad_ = prev_grad;
        calcAndUpdateGradient(newinfo);
        double cur_grad = prev_grad;

        // double relative_increase_ = (cur_grad - prev_grad_) / prev_grad_;
        double relative_increase_ = (cur_grad) / prev_grad_;
        return relative_increase_;
    }

    void
    OnTheFlyResponsePort::recvOtfInfo(OnTheFlyInfo *info)
    {
        // DPRINTF(AdaptiveDdioOtf, "recvOnTheFlyInfo\n");
        int qid = info->cache_id;

        grad_counter++;
        if (local_max_otf < info->otf_rate)
            local_max_otf = info->otf_rate;
        if (info->otf_rate < local_max_otf * 0.7)
            local_max_otf = info->otf_rate;
        if (grad_counter >= window_size)
        {
            grad_counter = 0;
            relative_increase = (calcGradChange(info));
            DPRINTF(AdaptiveDdioOtf, "recvOnTheFlyInfo; relative_increase %f\n", relative_increase);

            if (relative_increase < threshhold_gradchange && !lock_incrrate)
            {
                // local_max_otf = info->otf_rate;
                lock_incrrate = true;
            }
            // else{
            //     local_max_otf = 1;
            //     //lock_incrrate = false;
            // }
        }
        if (boost_cnt == 1000)
        {
            boost_cnt = 0;
            local_max_otf *= 2;
        }
        boost_cnt++;

        otfinfo.otf_rate = info->otf_rate;
        otfinfo.untouched_evict_rate = info->untouched_evict_rate;

        // DPRINTF(AdaptiveDdioOtfInfo, "AdaptiveDdioOtfInfo; Cache %s, OtfRate %f, untouched-evict %f, time %lld\n",
        //     info->cache_type == CACHE_TYPE_MLC ? "MLC" + std::to_string(qid) : "LLC",
        //     otfinfo.otf_rate, otfinfo.untouched_evict_rate, curTick() / sim_clock::as_int::ns);

        DPRINTF(AdaptiveDdioOtfInfo, "AdaptiveDdioOtfInfo; Cache %s, OtfRate %f, untouched-evict %f, relative_increase %f, local_max %f, time %lld\n",
                info->cache_type == CACHE_TYPE_MLC ? "MLC" + std::to_string(qid) : "LLC",
                otfinfo.otf_rate, otfinfo.untouched_evict_rate, relative_increase, local_max_otf, curTick() / sim_clock::as_int::ns);

        delete info;
    }

    void
    OnTheFlyResponsePort::Init(OnTheFlyInfo *info)
    {
        cache_id = info->cache_id;
        cache_type = info->cache_type;
        delete info;
    }

    OnTheFlyResponsePort *
    DdioBridge::findOtfPort(int mlcid, int type)
    {
        for (int i = 0; i < onTheFlyResponsePort.size(); i++)
        {
            OnTheFlyResponsePort *port = onTheFlyResponsePort[i];
            if (port->getCacheType() == type)
            {
                if (type == CACHE_TYPE_LLC)
                    return port;
                if (port->getCacheId() == mlcid)
                    return port;
            }
        }
        return nullptr;
    }

    bool
    OnTheFlyResponsePort::canSendToCache()
    {
        // 1. under otf th
        if (boost_cnt < 500)
            return true;
        float th = local_max_otf * threshhold_otf;
        DPRINTF(AdaptiveDdioOtfInfo, "canSendToCache th %f\n", th);
        if (prev_otf_rate < th)
        {
            // local_max_otf = prev_otf_rate;
            lock_incrrate = false;
            return true;
        }

        DPRINTF(AdaptiveDdioOtfInfo, "canSendToCache relative_increase %f\n", relative_increase);
        if (relative_increase > threshhold_gradchange)
        {
            // local_max_otf = prev_otf_rate;
            lock_incrrate = false;
            return true;
        }

        DPRINTF(AdaptiveDdioOtfInfo, "canSendToCache otfrate is %f\n", prev_otf_rate);
        if (prev_otf_rate < threshhold_abs)
        {
            // local_max_otf = prev_otf_rate;
            lock_incrrate = false;
            return true;
        }

        // 2.
        return false;
    }

    bool
    DdioBridge::canSendToObj(int adq, int dest)
    {
        if (adq > -1)
        {
            DPRINTF(AdaptiveDdioOtfInfo, "canSendToObj; adq %d, dest %d, ddio_option %d\n", adq, dest, ddio_option[adq]);
            return ddio_option[adq] & dest;
        }
        if (dest == allow_dram_only)
            return true;
        return false;
    }

    void
    OnTheFlyResponsePort::updateSendStatus()
    {
        // 1. OnTheFly > 10% --> TO_LLC
        if (prev_otf_rate > 0.1)
        {
            send_status = TO_LLC;
        }

        // 2. dataUsage < 10% --> TO_DRAM
        else if (dataUsage < 0.1)
        {
            send_status = TO_DRAM;
        }

        // 3. data
        else if (dataUsage > 0.9)
        {
            send_status = TO_MLC;
        }

        else
        {
            send_status = TO_LLC;
        }
    }
}