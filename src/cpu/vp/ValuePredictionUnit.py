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
    use_stride = Param.Bool(True, "Reset confidence to 0 on misprediction")


class EVES(ValuePredictor):
    type = "EVES"
    cxx_class = "gem5::EVES"
    cxx_header = "cpu/vp/eves.hh"

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
    use_stride = Param.Bool(True, "Reset confidence to 0 on misprediction")
