/*
 * Copyright (c) 2024 Purdue,UCSD I guess
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "arch/x86/regs/misc.hh"
#include "arch/x86/interrupts.hh"
#include "arch/x86/intmessage.hh"
#include "arch/x86/pcstate.hh"
#include "cpu/o3/cpu.hh"
#include "base/time.hh"
#include "debug/HackTimer.hh"
#include "hacktimer.hh"

namespace gem5
{

    namespace o3
    {

        HackTimer::HackTimer(CPU *cpu) : timerEvent([this]
                                                    { timer(); }, "Hack Timer Event",
                                                    false, Event::Default_Pri)
        {
            this->cpu = cpu;
        }

        void HackTimer::timer()
        {
            if (enabled)
            {
                reinterpret_cast<X86ISA::Interrupts *>(cpu->getInterruptController(0))->requestInterrupt(INTERRUPT_TIMER, X86ISA::delivery_mode::Fixed, false);
                cpu->schedule(timerEvent, cpu->clockEdge() + period);
            }
        }
        void HackTimer::setPeriod(Tick period_as_usecs)
        {
            double period_as_seconds = (double)period_as_usecs / 1e6;
            Time converter(period_as_seconds);
            DPRINTF(HackTimer, "The converter sec: %d, nsec: %d\n", converter.sec(), converter.nsec());
            this->period = converter.getTick();
            DPRINTF(HackTimer, "Setting period to %d\n", this->period);
            this->enabled = true;
            Tick nextTime = cpu->clockEdge() + this->period;
            DPRINTF(HackTimer, "Scheduling next timer event at %d, current at %d\n", nextTime, curTick());
            cpu->schedule(timerEvent, cpu->clockEdge() + period);
        }
        void HackTimer::disable()
        {
            this->enabled = false;
        }
    } // namespace o3
} // namespace gem5
