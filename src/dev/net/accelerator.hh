#ifndef __ACCELERATOR_HH__
#define __ACCELERATOR_HH__

#include "params/Accelerator.hh"
#include "dev/net/etherint.hh"
#include "sim/sim_object.hh"
#include "base/statistics.hh"
#include "sim/eventq.hh"
#include <random>
#include <fstream>

namespace gem5
{
    class AccelInt;
    struct AccelGen
    {

        double vmr;
        double max_error;
        std::default_random_engine generator;
        std::uniform_real_distribution<double> dist;
        AccelGen(double vmr, double max_error);
        Tick findNext(uint64_t packet_type);
        inline double NormalCDF(double value)
        {
            return 0.5 * erfc(-value * 1.4142135623730951);
        }
        // https://www.johndcook.com/blog/cpp_phi_inverse/
        inline double RationalApproximation(double t)
        {
            // Abramowitz and Stegun formula 26.2.23.
            // The absolute value of the error should be less than 4.5 e-4.
            double c[] = {2.515517, 0.802853, 0.010328};
            double d[] = {1.432788, 0.189269, 0.001308};
            return t - ((c[2] * t + c[1]) * t + c[0]) /
                           (((d[2] * t + d[1]) * t + d[0]) * t + 1.0);
        }

        inline double NormalCDFInverse(double p)
        {
            assert(p >= 0.0 && p <= 1.0);

            // See article above for explanation of this section.
            if (p < 0.5)
            {
                // F^-1(p) = - G^-1(p)
                return -RationalApproximation(sqrt(-2.0 * log(p)));
            }
            else
            {
                // F^-1(p) = G^-1(1-p)
                return RationalApproximation(sqrt(-2.0 * log(1 - p)));
            }
        }
    };
    class Accelerator : public SimObject
    {

        enum class Mode
        {
            Static,
            Normal
        };

    private:
        static constexpr unsigned MACHeaderSize = 14;
        Mode accelMode;
        AccelInt *interface;

        const uint8_t accelId;
        unsigned packetSize;
        AccelGen accelGen;
        uint64_t lastRxCount;
        uint64_t lastTxCount;
        struct AcceleratorStats : public statistics::Group
        {
            AcceleratorStats(statistics::Group *parent);
            statistics::Scalar sentPackets;
            statistics::Scalar recvPackets;
            statistics::Histogram latency;
        } AcceleratorStats;

    public:
        static inline bool switched = false;
        static Accelerator *self;
        std::ofstream latencies;
        Accelerator(const AcceleratorParams &p);
        static std::ofstream latency_file;

        Port &getPort(const std::string &if_name, PortID idx);
        void buildPacket(EthPacketPtr ethpacket, uint64_t sendTick, uint64_t req_type = 0);
        void sendPacket(uint64_t sendTick);
        bool processRxPkt(EthPacketPtr pkt);
        Tick findNext(uint64_t packetType);
    };

    class AccelInt : public EtherInt
    {
    private:
        Accelerator *dev;

    public:
        AccelInt(const std::string &name, Accelerator *d)
            : EtherInt(name), dev(d)
        {
        }

        virtual bool recvPacket(EthPacketPtr pkt) { return dev->processRxPkt(pkt); }
        virtual void sendDone() { return; }
    };
}
#endif // __ACCELERATOR_HH__