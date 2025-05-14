#ifndef __ARCH_X86_USERNOTIFICATION_HH__
#define __ARCH_X86_USERNOTIFICATION_HH__

#include "arch/x86/memhelpers.hh"
#include "arch/x86/regs/misc.hh"

namespace gem5
{

    namespace X86ISA
    {
        void updateUPID(ThreadContext *tc)
        {
            UintrTT uintrTT = tc->readMiscRegNoEffect(misc_reg::UintrTT);
            Addr uittAddr = ((uint64_t)(uintrTT.uittaddr)) << 4;
            // tc->getCpuPtr()->
        }
    } // namespace X86ISA
} // namespace gem5

#endif
