from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *
from m5.objects.Tags import *
from m5.params import *
from m5.SimObject import SimObject


class LoadClassificationTable(SimObject):
    type = "LoadClassificationTable"
    cxx_header = "cpu/lvp/load_classification_table.hh"
    cxx_class = "gem5::LoadClassificationTable"

    localPredictorSize = Param.Unsigned(512, "Size of local predictor")
    localCtrBits = Param.Unsigned(2, "Bits per counter")
    invalidateConstToZero = Param.Bool(
        False, "Reset counter to 0 on constant invalidation"
    )


class LoadValuePredictionTable(SimObject):
    type = "LoadValuePredictionTable"
    cxx_header = "cpu/lvp/load_value_prediction_table.hh"
    cxx_class = "gem5::LoadValuePredictionTable"

    entries = Param.Unsigned(
        1024, "Number of entries in the predicttion table"
    )
    historyDepth = Param.Unsigned(1, "History depth")


class ConstantVerificationUnit(SimObject):
    type = "ConstantVerificationUnit"
    cxx_header = "cpu/lvp/constant_verification_unit.hh"
    cxx_class = "gem5::ConstantVerificationUnit"
    entries = Param.Unsigned(8, "Number of entries in the CVU CAM")
    replacementPolicy = Param.Unsigned(
        1, "Replacement policy of the fully-assoc CAM"
    )


class LoadValuePredictionUnit(SimObject):
    type = "LoadValuePredictionUnit"
    cxx_header = "cpu/lvp/load_value_prediction_unit.hh"
    cxx_class = "gem5::LoadValuePredictionUnit"

    load_classification_table = Param.LoadClassificationTable(
        LoadClassificationTable(), "A load classification table"
    )
    load_value_prediction_table = Param.LoadValuePredictionTable(
        LoadValuePredictionTable(), "A load value prediction table"
    )
    constant_verification_unit = Param.ConstantVerificationUnit(
        ConstantVerificationUnit(), "A constant verification unit"
    )
    is_stride = Param.Bool(False, "Is this a stride predictor")
    is_context = Param.Bool(False, "Is this a context predictor")


class ValuePredictor(SimObject):
    type = "ValuePredictor"
    cxx_class = "gem5::ValuePredictor"
    cxx_header = "cpu/lvp/value_predictor.hh"
    abstract = True

    numThreads = Param.Unsigned(0, "Number of threads")
    instShiftAmt = Param.Unsigned(2, "Number of bits to shift instructions by")


class LVPStride(ValuePredictor):
    type = "LVPStride"
    cxx_class = "gem5::LVPStride"
    cxx_header = "cpu/lvp/lvp_stride.hh"

    tagBits = Param.Unsigned(16, "Tag bits of the load value predictor table")

    # table_entries = Param.Unsigned(8192, "Number of entries in the load value predictor table")
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
