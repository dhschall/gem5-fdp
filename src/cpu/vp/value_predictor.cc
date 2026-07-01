/*
 * Copyright (c) 2026 Technical Unversity of Munich
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "cpu/vp/value_predictor.hh"

namespace gem5
{

ValuePredictor::ValuePredictor(const ValuePredictorParams &params)
    : SimObject(params),
      numThreads(params.numThreads),
      instShiftAmt(params.instShiftAmt),
      stats(this)
{}

ValuePredictor::ValuePredictorStats::ValuePredictorStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of VP lookups"),
      ADD_STAT(misses, statistics::units::Count::get(),
               "Number of VP misses (No update available)"),
      ADD_STAT(predictableLoads, statistics::units::Count::get(),
               "Number of loads classified as predictable"),
      ADD_STAT(predicted, statistics::units::Count::get(),
               "Number of loads classified as predictable"),
      ADD_STAT(correct, statistics::units::Count::get(),
               "Number of loads correctly classified as predictable"),
      ADD_STAT(incorrect, statistics::units::Count::get(),
               "Number of loads incorrectly classified as predictable"),
      ADD_STAT(predCoverage, statistics::units::Count::get(),
               "Number covered loads predicted compared to all loads"),
      ADD_STAT(accuracy, statistics::units::Count::get(),
               "Accuracy of the predictions"),
      ADD_STAT(constLoads, statistics::units::Count::get(),
               "Number of loads classified as constant"),
      ADD_STAT(constLoadsCorrect, statistics::units::Count::get(),
               "Number of constant loads correctly predicted"),
      ADD_STAT(constLoadsIncorrect, statistics::units::Count::get(),
               "Number of constant loads incorrectly predicted"),
      ADD_STAT(totalLoads, statistics::units::Count::get(),
               "Total loads processed by the Load value predictor"),
      ADD_STAT(numZeroConstLoads, statistics::units::Count::get(),
               "Number of constant loads with value 0"),
      ADD_STAT(numOneConstLoads, statistics::units::Count::get(),
               "Number of constant loads with value 1")
{
    predicted = correct + incorrect;
    predCoverage = predicted / totalLoads;
    accuracy = correct / predicted;
}

} // namespace gem5
