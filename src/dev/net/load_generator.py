from m5.params import *
from m5.SimObject import SimObject
from m5.objects.Ethernet import EtherInt


class LoadGenerator(SimObject):
    type = 'LoadGenerator'
    cxx_header = "dev/net/load_generator.hh"
    cxx_class = 'gem5::LoadGenerator'

    interface = EtherInt("interface")
    start_tick =  Param.Tick(1,"Tick at whcih to start loadgenerator")
    stop_tick = Param.Tick(1, "Tick at which to stop loadgenerator")
    packet_size = Param.Int(64,"Packet size in bytes")
    packet_rate = Param.Int(100,"Number of packets per second to send")
    loadgen_id = Param.Int(0, "For match NIC")
    burst_width = Param.Tick(1, "Width of a packet burst in picoseconds")
    burst_gap = Param.Tick(1, "Time of gap between bursts in picoseconds")
    mode = Param.String("Increment", "LoadgenMode")