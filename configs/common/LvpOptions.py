# Robert Viramontes


from common import ObjectList

import m5
from m5.defines import buildEnv
from m5.objects import *


# Add the very basic options that work also in the case of the no ISA
# being used, and consequently no CPUs, but rather various types of
# testers and traffic generators.
def addLvpOptions(parser):
    # LCT params
    parser.add_argument("--lct-entries", default=512)
    parser.add_argument("--lct-ctr-bits", default=2)
    parser.add_argument("--lct-invalidate-zero", default=False)

    # LVPT params
    parser.add_argument("--lvpt-entries", default=1024)
    parser.add_argument("--lvpt-hist-depth", default=1)

    # CVU params
    parser.add_argument("--cvu-entries", default=8)
    parser.add_argument(
        "--cvu-replacement",
        default=1,
        help="1: FIFO, 2: LRU, 3: NLRU, 4: MRU, 5: NMRU",
    )

    # is there a LVP?
    parser.add_argument("--lvp", default=False)

    # want to do scalar instead?
    parser.add_argument("--scalar", default=False)

    # how many instructions should we be able to squash post mispredict?
    parser.add_argument("--squash_width", default=8)

    # is stride predictor?
    parser.add_argument("--stride", default=False)

    # is context predictor?
    parser.add_argument("--context", default=False)
