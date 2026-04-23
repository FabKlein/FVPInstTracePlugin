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
 * Title:        Coverage.cpp
 * Description:  InstProfiler::WriteCoverageJson() — per-function code coverage
 *               JSON output.
 *
 * $Date:        23 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/*
 * Coverage estimation method
 * --------------------------
 * For each function the plugin has collected the set of unique program counter
 * values (after Thumb-bit clearing) retired during the trace.  At shutdown
 * these sets are sorted and walked in address order to estimate how many bytes
 * of the function body were executed:
 *
 *   delta == 2  →  Thumb-16 instruction (2 bytes)
 *   delta == 4  →  Thumb-32 or ARM instruction (4 bytes)
 *   delta  > 4  →  assume 4 bytes; the gap means some PCs were never retired
 *                  (branch target, cold path …) — those bytes are NOT counted
 *   last PC     →  conservatively counted as 4 bytes (no successor to judge by)
 *
 * The estimated covered byte count is clamped to the symbol's address-range
 * size, and the resulting percentage is clamped to 100.0.
 *
 * Output format
 * -------------
 * A JSON array, one object per function that was entered at least once,
 * sorted by covered_bytes descending (most-executed first):
 *
 *   [
 *     {"name":"...", "mangled":"...",
 *      "addr":"0x...", "range_bytes":N,
 *      "unique_pcs":N, "covered_bytes":N, "coverage_pct":N.N},
 *     ...
 *   ]
 */

#include "InstProfiler.h"

#include <algorithm> // std::sort
#include <cstdio>    // fopen, fprintf, fclose
#include <vector>

// ==========================================================================
// ChromeTrace::WriteCoverageJson
// ==========================================================================

void InstProfiler::WriteCoverageJson()
{
    if (!coverage_enabled_ || coverage_map_.empty())
        return;

    struct CovEntry
    {
        const Symbol *sym;
        size_t   unique_pcs;
        uint64_t covered_bytes; ///< estimated from PC-delta heuristic
        double   coverage_pct;  ///< covered_bytes / range_bytes * 100, clamped to 100
    };

    std::vector<CovEntry> entries;
    entries.reserve(coverage_map_.size());

    for (const auto &kv : coverage_map_)
    {
        const Symbol *s = kv.first;
        const std::unordered_set<uint32_t> &pc_set = kv.second;

        // Sort unique PCs so we can walk them in address order.
        std::vector<uint32_t> sorted_pcs(pc_set.begin(), pc_set.end());
        std::sort(sorted_pcs.begin(), sorted_pcs.end());

        // Estimate covered bytes via delta heuristic.
        uint64_t covered = 0;
        for (size_t j = 0; j < sorted_pcs.size(); ++j)
        {
            uint32_t insn_size;
            if (j + 1 < sorted_pcs.size())
            {
                uint32_t delta = sorted_pcs[j + 1] - sorted_pcs[j];
                insn_size = (delta == 2) ? 2u : 4u;
            }
            else
            {
                insn_size = 4u; // last PC: assume 4 bytes
            }
            covered += insn_size;
        }

        const uint64_t range = (s->end > s->start) ? (s->end - s->start) : 0;
        if (range > 0 && covered > range)
            covered = range; // heuristic over-estimate: clamp to function size

        const double pct = (range > 0)
            ? (static_cast<double>(covered) * 100.0 / static_cast<double>(range))
            : 0.0;

        entries.push_back({s, sorted_pcs.size(), covered, pct < 100.0 ? pct : 100.0});
    }

    std::sort(entries.begin(), entries.end(),
              [](const CovEntry &a, const CovEntry &b) { return a.covered_bytes > b.covered_bytes; });

    FILE *cf = fopen(param_coverage_file_.c_str(), "w");
    if (!cf)
    {
        fprintf(stderr,
                "[InstProfiler] Could not open coverage file '%s' for writing.\n",
                param_coverage_file_.c_str());
        return;
    }

    fprintf(cf, "[\n");
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Symbol   *s     = entries[i].sym;
        const std::string &dname = Demangle(s->name);
        const uint64_t range  = (s->end > s->start) ? (s->end - s->start) : 0;
        fprintf(cf,
                "  {\"name\":\"%s\",\"mangled\":\"%s\","
                "\"addr\":\"0x%llx\",\"range_bytes\":%llu,"
                "\"unique_pcs\":%zu,\"covered_bytes\":%llu,"
                "\"coverage_pct\":%.1f}%s\n",
                dname.c_str(),
                s->name.c_str(),
                (unsigned long long)s->start,
                (unsigned long long)range,
                entries[i].unique_pcs,
                (unsigned long long)entries[i].covered_bytes,
                entries[i].coverage_pct,
                (i + 1 < entries.size()) ? "," : "");
    }
    fprintf(cf, "]\n");
    fclose(cf);

    printf("[InstProfiler] Coverage written to '%s' (%zu function(s))\n",
           param_coverage_file_.c_str(),
           entries.size());
}

// End of Coverage.cpp
