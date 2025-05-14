/**
 * Copyright (c) 2018-2020 Inria
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

#include "mem/cache/replacement_policies/lru_rp.hh"

#include <cassert>
#include <memory>

#include "params/LRURP.hh"
#include "sim/cur_tick.hh"

#include "debug/DDIO.hh"

namespace gem5
{

    namespace replacement_policy
    {

        LRU::LRU(const Params &p)
            : Base(p)
        {
        }

        void
        LRU::invalidate(const std::shared_ptr<ReplacementData> &replacement_data)
        {
            // Reset last touch timestamp
            std::static_pointer_cast<LRUReplData>(
                replacement_data)
                ->lastTouchTick = Tick(0);

            std::static_pointer_cast<LRUReplData>(
                replacement_data)
                ->ioInvalidated = false;
        }

        // SHIN

        void
        LRU::invalidateDDIO(const std::shared_ptr<ReplacementData> &replacement_data)
            const
        {
            // Reset last touch timestamp
            std::static_pointer_cast<LRUReplData>(
                replacement_data)
                ->lastTouchTick = Tick(0);

            std::static_pointer_cast<LRUReplData>(
                replacement_data)
                ->ioInvalidated = true;
        }

        void
        LRU::touch(const std::shared_ptr<ReplacementData> &replacement_data) const
        {
            // Update last touch timestamp
            std::static_pointer_cast<LRUReplData>(
                replacement_data)
                ->lastTouchTick = curTick();
        }

        void
        LRU::reset(const std::shared_ptr<ReplacementData> &replacement_data) const
        {
            // Set last touch timestamp
            std::static_pointer_cast<LRUReplData>(
                replacement_data)
                ->lastTouchTick = curTick();
        }

        ReplaceableEntry *
        LRU::getVictim(const ReplacementCandidates &candidates) const
        {
            // There must be at least one replacement candidate
            assert(candidates.size() > 0);

            // Visit all candidates to find victim
            ReplaceableEntry *victim = candidates[0];
            for (const auto &candidate : candidates)
            {
                // Update victim entry if necessary
                if (std::static_pointer_cast<LRUReplData>(
                        candidate->replacementData)
                        ->lastTouchTick <
                    std::static_pointer_cast<LRUReplData>(
                        victim->replacementData)
                        ->lastTouchTick)
                {
                    victim = candidate;
                }
            }

            return victim;
        }

        ReplaceableEntry *
        LRU::getVictimWayPart(const ReplacementCandidates &candidates,
                              int32_t way_part) const
        {
            // There must be at least one replacement candidate
            assert(candidates.size() > 0);

            uint32_t mask = 0x01;
            bool is_in_part = false;
            bool is_io_invalidated = false;
            if (is_io_invalidated)
            {
            }
            int way = 0;
            // Visit all candidates to find victim
            ReplaceableEntry *victim = candidates[0];
            // sanity check
            assert(victim->getWay() == 0);
            for (const auto &candidate : candidates)
            {
                assert(candidate->getWay() == way);
                if (std::static_pointer_cast<LRUReplData>(
                        candidate->replacementData)
                        ->ioInvalidated)
                {
                    victim = candidate;
                    is_io_invalidated = true;
                    std::static_pointer_cast<LRUReplData>(
                        candidate->replacementData)
                        ->ioInvalidated = false;
                    break;
                }

                if (mask & way_part)
                {
                    if (is_in_part == false)
                    {
                        victim = candidate;
                        is_in_part = true;
                    }

                    // Update victim entry if necessary
                    if (std::static_pointer_cast<LRUReplData>(
                            candidate->replacementData)
                            ->lastTouchTick <
                        std::static_pointer_cast<LRUReplData>(
                            victim->replacementData)
                            ->lastTouchTick)
                    {
                        victim = candidate;
                    }
                }
                way++;
                mask = mask << 1;
                assert(way < 32);
            }
            // DPRINTF(DDIO, "%s: victim way %d is_in_part %d is_io_invalidated %d\n", __func__, victim->getWay(), is_in_part, is_io_invalidated);
            // printf("getVictimWayPart: victim way %d is_in_part %d\n", victim->getWay(), is_in_part);

            return victim;
        }

        std::shared_ptr<ReplacementData>
        LRU::instantiateEntry()
        {
            return std::shared_ptr<ReplacementData>(new LRUReplData());
        }

    } // namespace replacement_policy
} // namespace gem5
