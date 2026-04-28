/*
 * SPDX-FileCopyrightText: Copyright 2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* ----------------------------------------------------------------------
 * Project:      Fast Models Portfolio - MTI Plugin
 * Title:        Flamegraph.cpp
 * Description:  InstProfiler::WriteFlamegraphFolded() — writes folded-stack
 *               output compatible with Brendan Gregg's flamegraph.pl.
 *
 * $Date:        28 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/*
 * Folded-stack format
 * -------------------
 * Each line is:   <stack_path> <count>\n
 *
 * where <stack_path> is a semicolon-separated list of function names from
 * root to leaf, and <count> is the number of "samples" (here: self-time in
 * raw instruction ticks).
 *
 * Example:
 *   main;process;compute 42000
 *   main;process;memcpy 8000
 *   main;init 1200
 *
 * To produce an SVG flamegraph:
 *   flamegraph.pl --countname="instruction ticks" folded.txt > flame.svg
 *
 * See: https://github.com/brendangregg/FlameGraph
 */

#include "InstProfiler.h"

#include <cstdio> // fopen, fprintf, fclose

// ==========================================================================
// InstProfiler::WriteFlamegraphFolded
// ==========================================================================

void InstProfiler::WriteFlamegraphFolded()
{
    if (!flamegraph_enabled_ || folded_stacks_.empty())
        return;

    FILE *ff = fopen(param_flamegraph_file_.c_str(), "w");
    if (!ff)
    {
        fprintf(stderr,
                "[InstProfiler] Could not open flamegraph file '%s' for writing.\n",
                param_flamegraph_file_.c_str());
        return;
    }

    // The map is std::map<string, uint64_t>, so output is already sorted
    // alphabetically by stack path — convenient for diffing/grepping.
    for (const auto &kv : folded_stacks_)
        fprintf(ff, "%s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);

    fclose(ff);

    printf("[InstProfiler] Flamegraph folded stacks written to '%s' (%zu unique path(s))\n",
           param_flamegraph_file_.c_str(),
           folded_stacks_.size());
}
// End of Flamegraph.cpp
