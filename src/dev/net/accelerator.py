from m5.params import *
from m5.SimObject import SimObject
from m5.objects.Ethernet import EtherInt


class Accelerator(SimObject):
    type = 'Accelerator'
    cxx_header = "dev/net/accelerator.hh"
    cxx_class = 'gem5::Accelerator'

    interface = EtherInt("interface")
    packet_size = Param.Int(64,"Packet size in bytes")
    accel_id = Param.Int(0, "For match NIC")
    vmr = Param.Float(1, "Variance Mean Ratio")
    max_error = Param.Float(2, "Max Error")
    mode = Param.String("Increment", "AccelServiceDistribution")