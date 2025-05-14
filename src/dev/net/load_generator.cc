#include "dev/net/load_generator.hh"
#include "dev/net/i8254xGBe.hh"
#include "dev/x86/pc.hh"
#include "cpu/o3/cpu.hh"
#include <inttypes.h>
#include "sim/sim_exit.hh"
#include <netinet/in.h>
#include "base/trace.hh"
#include "debug/LoadgenDebug.hh"
#include "debug/LoadgenLatency.hh"
#include "load_generator.hh"

namespace gem5
{
    std::vector<LoadGenerator *> LoadGenerator::self = {};
    std::ofstream LoadGenerator::latencies = std::ofstream("latency.txt", std::ofstream::out | std::ofstream::trunc);
    LoadGenerator::LoadGeneratorStats::LoadGeneratorStats(statistics::Group *parent)
        : statistics::Group(parent, "LoadGenerator"),
          ADD_STAT(sentPackets, statistics::units::Count::get(), "Number of Generated Packets"),
          ADD_STAT(recvPackets, statistics::units::Count::get(), "Number of Recieved Packets"),
          ADD_STAT(latency, statistics::units::Second::get(), "Distribution of Latency in ms")
    {
        sentPackets.precision(0);
        recvPackets.precision(0);
        latency.init(100);
    }

    LoadGenerator::LoadGenerator(const LoadGeneratorParams &p) : SimObject(p), loadgenId(p.loadgen_id), packetSize(p.packet_size), packetRate(p.packet_rate),
                                                                 startTick(p.start_tick), stopTick(p.stop_tick), checkLossInterval(1000), incrementInterval(5e+8 / (packetSize * 8)), // Whats a good value for this?
                                                                 burstWidth(p.burst_width), burstGap(p.burst_gap), burstStartTick(0), poissonGenerator(p.packet_rate, p.loadgen_id),
                                                                 lastRxCount(0), lastTxCount(0),
                                                                 sendPacketEvent([this]
                                                                                 { sendPacket(); },
                                                                                 name()),
                                                                 checkLossEvent([this]
                                                                                { checkLoss(); },
                                                                                name()),
                                                                 loadGeneratorStats(this)
    {
        if (p.mode == "Static")
            loadgenMode = Mode::Static;
        else if (p.mode == "Increment")
            loadgenMode = Mode::Increment;
        else if (p.mode == "Burst")
            loadgenMode = Mode::Burst;
        else if (p.mode == "Poisson")
            loadgenMode = Mode::Poisson;
        else if (p.mode == "Alternating Poisson")
        {
            loadgenMode = Mode::Alternating;
            poissonGenerator.high_portion = burstGap;
        }
        endTick = stopTick;
        LoadGenerator::interface = new LoadGenInt("interface", this);
        LoadGenerator::self.push_back(this);
    }

    Tick LoadGenerator::frequency()
    {
        return (1e12 / packetRate);
    }

    void LoadGenerator::startup()
    {
        /*if (curTick() > stopTick)
            return;

        if (curTick() > startTick)
            schedule(sendPacketEvent, curTick() + 1);
        else
            schedule(sendPacketEvent, startTick + 1);*/
    }

    Port &LoadGenerator::getPort(const std::string &if_name, PortID idx)
    {
        return *interface;
    }

    void LoadGenerator::buildPacket(EthPacketPtr ethpacket, uint64_t req_type)
    {
        // Build Packet header
        // DSTMAC 6 | SRCMAC 6 | LENGTH 2 | DATA
        uint8_t dst_mac[6] = {0x00, 0x80, 0x00, 0x00, 0x00, 0x01 + loadgenId}; // Use paired NIC's MAC
        uint8_t src_mac[6] = {0x00, 0x90, 0x00, 0x00, 0x00, 0x01 + loadgenId};

        uint16_t size = ethpacket->length;

        if (1 != htons(1))
            size = htons(size);

        uint8_t head[MACHeaderSize];
        memcpy(head, dst_mac, 6);
        memcpy(head + 6, src_mac, 6);
        memcpy(head + 12, &size, 2);
        memcpy(ethpacket->data, head, MACHeaderSize);
        uint64_t timeStamp = o3::cpu_to_find->eventQueue()->getCurTick();
        memcpy(&(ethpacket->data[MACHeaderSize]), &timeStamp, sizeof(uint64_t));
        memcpy(&(ethpacket->data[MACHeaderSize + sizeof(uint64_t)]), &req_type, sizeof(uint64_t));
    }

    void LoadGenerator::sendPacket()
    {
        if (!switched)
        {
            schedule(sendPacketEvent, curTick() + 10000);
            return;
        }

        if (first_send_call)
        {
            first_send_call = false;
            if (packetRate == 0)
            {
                return;
            }
        }
        else
        {
            if (!LoadGenerator::stopCounting || curTick() < LoadGenerator::stopCounting)
            {
                loadGeneratorStats.sentPackets++;
                lastTxCount++;
            }
            EthPacketPtr txPacket = std::make_shared<EthPacketData>(packetSize);
            txPacket->length = packetSize;
            buildPacket(txPacket);
            interface->sendPacket(txPacket);
        }

        if (curTick() < stopTick)
        {
            if (loadgenMode == Mode::Increment)
            {
                if (lastTxCount == checkLossInterval)
                    // allow enough time for any in flight packets to be recieved
                    schedule(checkLossEvent, curTick() + 100000000);
                else
                    schedule(sendPacketEvent, curTick() + frequency());
            }
            else if (loadgenMode == Mode::Static)
            {
                schedule(sendPacketEvent, curTick() + frequency());
            }
            else if (loadgenMode == Mode::Burst)
            {
                if (curTick() - burstStartTick > burstWidth)
                {
                    burstStartTick = curTick() + burstGap;
                    DPRINTF(LoadgenDebug, "Burst Ended, next Burst Starts at %lu \n", burstStartTick);
                    schedule(sendPacketEvent, burstStartTick);
                }
                else
                    schedule(sendPacketEvent, curTick() + frequency());
            }
            else if (loadgenMode == Mode::Poisson || loadgenMode == Mode::Alternating)
            {
                DPRINTF(LoadgenDebug, "Sent packet \n");
                Tick nextPacketTick = curTick() + poissonGenerator.nextPacketDelay();
                if (nextPacketTick < endTick)
                {
                    schedule(sendPacketEvent, nextPacketTick);
                }
            }
        }
    }

    void LoadGenerator::checkLoss()
    {
        // because we don't checkpoint uints wrap around because loadgen starts before dpdk
        if (lastTxCount - lastRxCount < 10 || lastRxCount > lastTxCount)
        {
            packetRate = packetRate + incrementInterval;
            schedule(sendPacketEvent, curTick() + frequency());
            DPRINTF(LoadgenDebug, "Rate Incremented, now sending packets at %u \n", packetRate);
            DPRINTF(LoadgenDebug, "Rx %lu, Tx %lu \n", lastRxCount, lastTxCount);
        }
        else
        {
            if ((packetRate - incrementInterval) < packetRate)
                packetRate = packetRate - incrementInterval;

            // add extra delay to prevent previouse loss from affecting results
            schedule(sendPacketEvent, curTick() + frequency() + 100000000);
            DPRINTF(LoadgenDebug, "Loss Detected, now sending packets at %u \n", packetRate);
            DPRINTF(LoadgenDebug, "Rx %lu, Tx %lu \n", lastRxCount, lastTxCount);
        }
        lastTxCount = 0;
        lastRxCount = 0;
    }

    void LoadGenerator::endTest()
    {
        DPRINTF(LoadgenDebug, "Ending test\n");
        exitSimLoop("m5_exit by loadgen End Simulator.", 0, curTick(), 0, true);
    }

    bool LoadGenerator::processRxPkt(EthPacketPtr pkt)
    {

        uint64_t sendTick;
        memcpy(&sendTick, &(pkt->data[14]), sizeof(uint64_t));
        if (!sendTick)
        {
            return true;
        }
        float delta = float((o3::cpu_to_find->eventQueue()->getCurTick() - sendTick)) / 1e3;
        if (LoadGenerator::startCounting <= curTick())
        {
            lastRxCount++;
            loadGeneratorStats.recvPackets++;
	
			latencies << std::flush;
            latencies << delta << std::endl;
			latencies << std::flush;
        }
        // std::open(); packets

        loadGeneratorStats.latency.sample(delta);
        DPRINTF(LoadgenLatency, "Latency %f \n", delta);
        return true;
    }
    PoissonGenerator::PoissonGenerator(unsigned packet_rate, int seed) : generator(seed), generator_low(1), distribution((double)packet_rate), distribution_low((double)4.0 * 89616.0), state_edge(curTick())
    {
    }

    Tick PoissonGenerator::nextPacketDelay()
    {
        if (high_portion != 10 && curTick() > state_edge + 1000000 + (counter) * 1000000)
        {
            counter++;
            if (counter == high_portion)
            {
                state = State::Low;
            }
            if (counter == 10)
            {
                state = State::High;
                counter = 0;
                state_edge = state_edge + 10 * 1000000;
            }
        }
        if (state == State::High)
        {
            return nextHighPacketDelay();
        }
        return nextLowPacketDelay();
    }
    Tick PoissonGenerator::nextHighPacketDelay()
    {
        double packet_value = distribution(generator) * 1e12;
        // DPRINTF(LoadgenLatency, "Next Packet will be scheduled ")
        return Tick(std::ceil(packet_value));
    }
    Tick PoissonGenerator::nextLowPacketDelay()
    {
        double packet_value = distribution_low(generator_low) * 1e12;
        // DPRINTF(LoadgenLatency, "Next Packet will be scheduled ")
        return Tick(std::ceil(packet_value));
    }
}
