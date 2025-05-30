# gem5.opt
BASEDIR=/path/to/gem5
# kernel
KDIR=/path/to/kernel
# checkpoint
CKPTDIR=/path/to/checkpoint
# image
IMGDIR=/path/to/img
# output
OUTDIR=/path/to/output
mkdir $OUTDIR

$BASEDIR/build/X86/gem5.opt \
-r -e --debug-file=m5.debug.log --listener-mode=off \
--outdir=$OUTDIR \
$BASEDIR/configs/example/fs_exp.py \
--kernel=$KDIR \
--eip \
--caches --l2cache --l3cache \
-r=1 --decouple --checkpoint-dir=$CKPTDIR \
--restore-with-cpu=X86O3CPU --cpu-type=X86O3CPU \
-n=1 --disk-image=$IMGDIR
