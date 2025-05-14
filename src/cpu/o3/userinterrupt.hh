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
#ifndef __CPU_O3_USERINTERRUPT_HH__
#define __CPU_O3_USERINTERRUPT_HH__
#include "base/types.hh"
#include "arch/generic/pcstate.hh"

namespace gem5
{

    namespace o3
    {
        class CPU;
        class UserInterruptProcessor
        {
        public:
            bool checkInterruptReady() const { return userInt != NoFault && issue && !waitingForBoundary && !frozen; }
            bool checkInterrupt() const { return userInt != NoFault; }
            bool checkFlags() const;
            bool checkIfOngoingNotification() const;
            bool checkIfOngoing() const;
            void setInterrupt(Fault userInt, uint64_t vector, Addr pc);
            void setInterrupt(Addr pc);
            void resetInterrupt();
            std::unique_ptr<PCStateBase> processInterrupt();
            void setWait();
            void resetWait();

            bool shouldWait() const
            {
                return waitingForBoundary;
            }
            void freeze();
            void unFreeze();
            bool isFrozen() const { return frozen; }

            void setIssue() { issue = true; }
            UserInterruptProcessor(CPU *cpu);

        public:
            bool pcUsable = false;

        private:
            Fault userInt;
            Addr returnPC;
            uint64_t vector;
            CPU *cpu;
            bool issue = false;
            bool waitingForBoundary = false;
            bool frozen = false;
            bool ongoing = false;
            bool apicreset = false;

            uint64_t arrival = 0;
            uint64_t retire = 0;
        };

    } // namespace o3
} // namespace gem5
#endif // __CPU_O3_USERINTERRUPT_HH__