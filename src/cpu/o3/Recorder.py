from m5.params import *
from m5.proxy import *
from m5.SimObject import *

from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *

class Recorder(SimObject):
    type = "Recorder"
    cxx_header = "cpu/o3/recorder.hh"
    cxx_class = "gem5::Recorder"
    cxx_exports = [PyBindMethod("addCaches")]

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._l1icaches = []
        self._l2caches = []
    def regProbeListeners(self):
        self.getCCObject().addCaches(self._l1icaches[0].getCCObject(), self._l2caches[0].getCCObject())
        self.getCCObject().regProbeListeners()
    def registerCaches(self, l1i, l2):
        if not isinstance(l1i, SimObject) or not isinstance(l2, SimObject):
            raise TypeError("argument must be a SimObject type")
        self._l1icaches.append(l1i)
        self._l2caches.append(l2)

    recorder_port = RequestPort("to L3")
    fetchBufferSize = Param.Unsigned(64, "Fetch buffer size in bytes")
    log_block_size = Param.Unsigned(6, "log2 of cache line size")

    index_entries = Param.MemorySize("512", "Number of entries in the index")
    index_assoc = Param.Unsigned(16, "Associativity of the index")
    index_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1, assoc=Parent.index_assoc, size=Parent.index_entries
        ),
        "Indexing policy of the index",
    )
    index_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the index"
    )
