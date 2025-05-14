#include "dev/net/accelerator.hh"
#include "dev/net/i8254xGBe.hh"
#include "dev/x86/pc.hh"
#include "cpu/o3/cpu.hh"
#include <inttypes.h>
#include "sim/sim_exit.hh"
#include <netinet/in.h>
#include "base/trace.hh"
#include "debug/AccelDebug.hh"
#include "accelerator.hh"
namespace gem5
{
    Accelerator *Accelerator::self = nullptr;
    std::ofstream Accelerator::latency_file("accel_latency");
    Accelerator::AcceleratorStats::AcceleratorStats(statistics::Group *parent)
        : statistics::Group(parent, "Accelerator"),
          ADD_STAT(sentPackets, statistics::units::Count::get(), "Number of Generated Packets"),
          ADD_STAT(recvPackets, statistics::units::Count::get(), "Number of Recieved Packets"),
          ADD_STAT(latency, statistics::units::Second::get(), "Distribution of Latency in ms")
    {
        sentPackets.precision(0);
        recvPackets.precision(0);
        latency.init(100);
    }

    Accelerator::Accelerator(const AcceleratorParams &p) : SimObject(p), accelId(p.accel_id), packetSize(p.packet_size),
                                                           lastRxCount(0), lastTxCount(0),
                                                           accelGen(p.vmr, p.max_error),
                                                           AcceleratorStats(this)
    {
        if (p.mode == "Static")
            accelMode = Mode::Static;
        else if (p.mode == "Normal")
            accelMode = Mode::Normal;
        Accelerator::interface = new AccelInt("interface", this);
        Accelerator::self = this;
    }

    Port &Accelerator::getPort(const std::string &if_name, PortID idx)
    {
        return *interface;
    }

    void Accelerator::buildPacket(EthPacketPtr ethpacket, uint64_t sendTick, uint64_t req_type)
    {
        // Build Packet header
        // DSTMAC 6 | SRCMAC 6 | LENGTH 2 | DATA
        uint8_t dst_mac[6] = {0x00, 0x90, 0x00, 0x00, 0x00, 0x01 + accelId}; // Use paired NIC's MAC
        uint8_t src_mac[6] = {0x00, 0x80, 0x00, 0x00, 0x00, 0x01 + accelId};

        uint16_t size = ethpacket->length;

        if (1 != htons(1))
            size = htons(size);

        uint8_t head[MACHeaderSize];
        memcpy(head, dst_mac, 6);
        memcpy(head + 6, src_mac, 6);
        memcpy(head + 12, &size, 2);
        memcpy(ethpacket->data, head, MACHeaderSize);
        memcpy(&(ethpacket->data[MACHeaderSize]), &sendTick, sizeof(uint64_t));
        memcpy(&(ethpacket->data[MACHeaderSize + sizeof(uint64_t)]), &req_type, sizeof(uint64_t));
    }

    void Accelerator::sendPacket(uint64_t sendTick)
    {
        AcceleratorStats.sentPackets++;
        lastTxCount++;
        EthPacketPtr txPacket = std::make_shared<EthPacketData>(packetSize);
        txPacket->length = packetSize;
        buildPacket(txPacket, sendTick);
        interface->sendPacket(txPacket);
    }

    bool Accelerator::processRxPkt(EthPacketPtr pkt)
    {

        uint64_t sendTick;
        uint64_t packetType;
        memcpy(&sendTick, &(pkt->data[14]), sizeof(uint64_t));
        memcpy(&packetType, &(pkt->data[14 + sizeof(uint64_t)]), sizeof(uint64_t));
        AcceleratorStats.recvPackets++;
        lastRxCount++;
        // std::open(); packets
        EventFunctionWrapper *sendPacketEventPtr = new EventFunctionWrapper([sendTick, this]()
                                                                            { sendPacket(sendTick); }, name(), true);
        schedule(sendPacketEventPtr, curTick() + findNext(packetType));
        return true;
    }
    static inline Tick usToTicks(double us)
    {
        return std::round(us * 1e6);
    }
    Tick Accelerator::findNext(uint64_t packetType)
    {
        switch (accelMode)
        {
        case Mode::Static:
            return usToTicks(packetType);
        case Mode::Normal:
            return accelGen.findNext(packetType);
        }
    }
    AccelGen::AccelGen(double vmr, double max_error) : vmr(vmr), max_error(max_error), generator(0), dist()
    {
    }
    Tick AccelGen::findNext(uint64_t packet_type)
    {
        double mean = packet_type;
        double variance = vmr * mean;
        double stddev = std::sqrt(variance);
        double beta = max_error / stddev;
        double alpha = -beta;
        double phi_alpha = NormalCDF(alpha);
        double phi_beta = NormalCDF(beta);
        double U = dist(generator);
        double val = phi_alpha + U * (phi_beta - phi_alpha);
        if (val < 0.0 && val >= -1e-3)
        {
            val = 1e-7;
        }
        if (val > 1.0 && val <= 1 + 1e-3)
        {
            val = 1.0 - 1e-7;
        }
        if (val < 0.0 || val > 1.0)
        {
            std::cerr << "we are about to error, phi_alpha:" << phi_alpha << " phi_beta:" << phi_beta << std::endl;
        }
        double next = NormalCDFInverse(phi_alpha + U * (phi_beta - phi_alpha)) * stddev + mean;
        return usToTicks(next);
    }
}
