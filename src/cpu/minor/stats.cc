/*
 * Copyright (c) 2012-2014 ARM Limited
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
 *
 * Authors: Andrew Bardsley
 */

#include "cpu/minor/stats.hh"

namespace Minor
{

MinorStats::MinorStats()
{ }

void
MinorStats::regStats(const std::string &name, BaseCPU &baseCpu)
{
    numInsts
        .name(name + ".committedInsts")
        .desc("Number of instructions committed");
numUnnecessaryInst
        .name(name + ".unnecessaryCommittedInsts")
        .desc("Number of SWIFT/ZDC unnecessary instructions committed");

    numOps
        .name(name + ".committedOps")
        .desc("Number of ops (including micro ops) committed");

    numDiscardedOps
        .name(name + ".discardedOps")
        .desc("Number of ops (including micro ops) which were discarded "
            "before commit");

    numFetchSuspends
        .name(name + ".numFetchSuspends")
        .desc("Number of times Execute suspended instruction fetching");

    quiesceCycles
        .name(name + ".quiesceCycles")
        .desc("Total number of cycles that CPU has spent quiesced or waiting "
              "for an interrupt")
        .prereq(quiesceCycles);

    cpi
        .name(name + ".cpi")
        .desc("CPI: cycles per instruction")
        .precision(6);
    cpi = baseCpu.numCycles / numInsts;

    ipc
        .name(name + ".ipc")
        .desc("IPC: instructions per cycle")
        .precision(6);
    ipc = numInsts / baseCpu.numCycles;

    tickCyclesMain
        .name(name + ".tickCyclesMain")
        .desc("Number of cycles which we spend in Main functions");
Inst0InIQ

                                .name(name + ".Inst0InIQ")
                                .desc("The number of tickes that there is 0 instruction in IQ.");
Inst1InIQ

                                .name(name + ".Inst1InIQ")
                                .desc("The number of tickes that there is 1 instruction in IQ.");
Inst2InIQ

                                .name(name + ".Inst2InIQ")
                                .desc("The number of tickes that there is 2 instruction in IQ.");
Inst3InIQ

                                .name(name + ".Inst3InIQ")
                                .desc("The number of tickes that there is 3 instruction in IQ.");
Inst4InIQ

                                .name(name + ".Inst4InIQ")
                                .desc("The number of tickes that there is 4 instruction in IQ.");
Inst5InIQ

                                .name(name + ".Inst5InIQ")
                                .desc("The number of tickes that there is 5 instruction in IQ.");
Inst6InIQ

                                .name(name + ".Inst6InIQ")
                                .desc("The number of tickes that there is 6 instruction in IQ.");
Inst7InIQ

                                .name(name + ".Inst7InIQ")
                                .desc("The number of tickes that there is 7 instruction in IQ.");
//LSQ
Inst0InLSQ

                                .name(name + ".Inst0InLSQ")
                                .desc("The number of tickes that there is 0 instruction in LSQ.");
Inst1InLSQ

                                .name(name + ".Inst1InLSQ")
                                .desc("The number of tickes that there is 1 instruction in LSQ.");
Inst2InLSQ

                                .name(name + ".Inst2InLSQ")
                                .desc("The number of tickes that there is 2 instruction in LSQ.");
Inst3InLSQ

                                .name(name + ".Inst3InLSQ")
                                .desc("The number of tickes that there is 3 instruction in LSQ.");
Inst4InLSQ

                                .name(name + ".Inst4InLSQ")
                                .desc("The number of tickes that there is 4 instruction in LSQ.");
Inst5InLSQ

                                .name(name + ".Inst5InLSQ")
                                .desc("The number of tickes that there is 5 instruction in LSQ.");
Inst6InLSQ

                                .name(name + ".Inst6InLSQ")
                                .desc("The number of tickes that there is 6 instruction in LSQ.");
Inst7InLSQ

                                .name(name + ".Inst7InLSQ")
                                .desc("The number of tickes that there is 7 instruction in LSQ.");
Inst8InLSQ

                                .name(name + ".Inst8InLSQ")
                                .desc("The number of tickes that there is 8 instruction in LSQ.");
FU0isBusy
                                .name(name + ".FU0isBusy")
                                .desc("The number of tickes that functional unit 0 was busy.");
FU1isBusy
                                .name(name + ".FU1isBusy")
                                .desc("The number of tickes that functional unit 1 was busy.");
FU2isBusy
                                .name(name + ".FU2isBusy")
                                .desc("The number of tickes that functional unit 2 was busy.");
FU3isBusy
                                .name(name + ".FU3isBusy")
                                .desc("The number of tickes that functional unit 3 was busy.");
FU4isBusy
                                .name(name + ".FU4isBusy")
                                .desc("The number of tickes that functional unit 4 was busy.");
FU5isBusy
                                .name(name + ".FU5isBusy")
                                .desc("The number of tickes that functional unit 5 was busy.");
FU6isBusy
                                .name(name + ".FU6isBusy")
                                .desc("The number of tickes that functional unit 6 was busy.");

}

};
