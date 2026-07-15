from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *
from m5.objects.Tags import *
from m5.params import *
from m5.SimObject import SimObject


class ValuePredictor(SimObject):
    type = "ValuePredictor"
    cxx_class = "gem5::ValuePredictor"
    cxx_header = "cpu/vp/value_predictor.hh"
    abstract = True

    numThreads = Param.Unsigned(0, "Number of threads")
    instShiftAmt = Param.Unsigned(2, "Number of bits to shift instructions by")


class StrideLVP(ValuePredictor):
    type = "StrideLVP"
    cxx_class = "gem5::StrideLVP"
    cxx_header = "cpu/vp/stride_lvp.hh"

    tagBits = Param.Unsigned(16, "Tag bits of the load value predictor table")

    table_entries = Param.MemorySize(
        "8192", "Number of entries in the load value predictor table"
    )
    table_assoc = Param.Unsigned(8, "Associativity of the predictor table")
    table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.table_assoc,
            size=Parent.table_entries,
        ),
        "Indexing policy of the load value prediction table",
    )
    table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the PC table"
    )

    confidence_threshold = Param.Unsigned(
        2, "Confidence threshold for predictions"
    )
    confidence_reset_to_zero = Param.Bool(
        False, "Reset confidence to 0 on misprediction"
    )
    use_stride = Param.Bool(True, "Use stride (true) or use constant (false)")


class StrideAvppLVP(ValuePredictor):
    type = "StrideAvppLVP"
    cxx_class = "gem5::StrideAvppLVP"
    cxx_header = "cpu/vp/stride_avpp_lvp.hh"

    pdis_max_value = Param.Unsigned(8, "Max prefetch distance")

    # -----------------
    # AT
    # -----------------
    at_table_entries = Param.MemorySize(
        "512", "Number of entries in the address table"
    )
    at_table_assoc = Param.Unsigned(1, "Associativity of the value table")
    at_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.at_table_assoc,
            size=Parent.at_table_entries,
        ),
        "Indexing policy of the value table",
    )
    at_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the value table"
    )

    # -----------------
    # VT
    # -----------------
    vt_table_entries = Param.MemorySize(
        "64", "Number of entries in the value table"
    )
    vt_table_assoc = Param.Unsigned(1, "Associativity of the value table")
    vt_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.vt_table_assoc,
            size=Parent.vt_table_entries,
        ),
        "Indexing policy of the value table",
    )
    vt_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the value table"
    )

    confidence_threshold = Param.Unsigned(
        2, "Confidence threshold for predictions"
    )
    confidence_reset_to_zero = Param.Bool(
        False, "Reset confidence to 0 on misprediction"
    )

    use_stride = Param.Bool(True, "Use stride (true) or use constant (false)")

    prob_up = Param.Unsigned(
        3,
        "The probability, expressed like 1/2^P, of incrementing or decreasing the prefetch distance. Therefore 1 means 1/2, 2 means 1/4, ...",
    )

    prefetch_request_port = RequestPort(
        "Port for requesting speculative prefetch of the VT table."
    )

    size_prefetch_block_queue = Param.Unsigned(
        2,
        "How many prefetchs can be in a state of not-being able to be sent at the same time to memory.",
    )
