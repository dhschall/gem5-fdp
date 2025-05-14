#ifndef __LOAD_GENERATOR_HH__
#define __LOAD_GENERATOR_HH__

#include "params/LoadGenerator.hh"
#include "dev/net/etherint.hh"
#include "sim/sim_object.hh"
#include "base/statistics.hh"
#include "sim/eventq.hh"
#include <random>
#include <fstream>

namespace gem5
{
    class LoadGenInt;
    class PoissonGenerator
    {
        std::default_random_engine generator;
        std::default_random_engine generator_low;
        std::exponential_distribution<double> distribution;
        std::exponential_distribution<double> distribution_low;
        enum State
        {
            Low,
            High
        };
        State state = State::High;
        Tick state_edge;
        uint8_t counter = 0;

    public:
        PoissonGenerator(unsigned packet_rate, int seed);
        Tick nextPacketDelay();
        Tick nextHighPacketDelay();
        Tick nextLowPacketDelay();
        uint8_t high_portion = 10;
    };
    class LoadGenerator : public SimObject
    {

        enum class Mode
        {
            Static,
            Increment,
            Burst,
            Poisson,
            Alternating
        };

    private:
        static constexpr unsigned MACHeaderSize = 14;
        Mode loadgenMode;
        LoadGenInt *interface;

        void checkLoss();
        Tick frequency();
        void endTest();
        const uint8_t loadgenId;
        unsigned packetSize;
        unsigned packetRate;
        const Tick startTick;
        const Tick stopTick;
        const unsigned checkLossInterval;
        Tick incrementInterval;
        Tick burstWidth;
        Tick burstGap;
        Tick burstStartTick;
        PoissonGenerator poissonGenerator;
        uint64_t lastRxCount;
        uint64_t lastTxCount;
        EventFunctionWrapper sendPacketEvent;
        EventFunctionWrapper checkLossEvent;
        struct LoadGeneratorStats : public statistics::Group
        {
            LoadGeneratorStats(statistics::Group *parent);
            statistics::Scalar sentPackets;
            statistics::Scalar recvPackets;
            statistics::Histogram latency;
        } loadGeneratorStats;

    public:
        static inline bool switched = false;
        static inline Tick endTick = 0;
        static inline Tick stopCounting = 0;
        static inline Tick startCounting = -1;
        static inline Tick firstTick = 0;
        static std::vector<LoadGenerator *> self;
        static std::ofstream latencies;
        bool first_send_call = true;
        LoadGenerator(const LoadGeneratorParams &p);

        Port &getPort(const std::string &if_name, PortID idx);
        void buildPacket(EthPacketPtr ethpacket, uint64_t req_type = 0);
        void startup();
        void sendPacket();
        bool processRxPkt(EthPacketPtr pkt);
    };

    class LoadGenInt : public EtherInt
    {
    private:
        LoadGenerator *dev;

    public:
        LoadGenInt(const std::string &name, LoadGenerator *d)
            : EtherInt(name), dev(d)
        {
        }

        virtual bool recvPacket(EthPacketPtr pkt) { return dev->processRxPkt(pkt); }
        virtual void sendDone() { return; }
    };
}
#endif // __LOAD_GENERATOR_HH__