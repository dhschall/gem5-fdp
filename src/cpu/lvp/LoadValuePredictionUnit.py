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
