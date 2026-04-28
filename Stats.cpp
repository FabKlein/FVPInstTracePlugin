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
 * Title:        Stats.cpp
 * Description:  InstProfiler::WriteStatsCSV() — per-function self/wall-time
 *               performance statistics CSV output.
 *
 * $Date:        23 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/*
 * Statistics method
 * -----------------
 * As each call-stack frame is popped in EmitFrame(), two time values are
 * accumulated per symbol:
 *
 *   wall_sum_us  — total time from function entry to return, including time
 *                  spent in all callees.  Equivalent to Python cProfile
 *                  "cumtime".
 *
 *   self_sum_us  — wall_sum_us minus time attributed to direct callees.
 *                  Equivalent to Python cProfile "tottime".  Computed as:
 *                      self_ticks = wall_ticks - frame.callee_clock
 *                  where callee_clock is propagated up through PopUntil().
 *
 * Both values are in the same microsecond units as the JSON trace
 * (i.e., INST_COUNT / time-scale).
 *
 * Output format
 * -------------
 * A CSV file with a header row, sorted by self_sum_us descending
 * (hottest self-time first):
 *
 *   name,count,wall_sum_us,self_sum_us,wall_avg_us,self_avg_us
 *   "my_function",1024,18432.000,14208.000,18.000,13.875
 *   ...
 *
 * Names are wrapped in double-quotes and embedded double-quotes are escaped
 * by doubling them, per CSV rules.
 */

#include "InstProfiler.h"

#include <algorithm> // std::sort
#include <cstdio>    // fopen, fprintf, fclose
#include <vector>

namespace {

std::string EscapeCsv(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value)
    {
        if (ch == '"')
            escaped += "\"\"";
        else
            escaped += ch;
    }
    return escaped;
}

} // namespace

// ==========================================================================
// ChromeTrace::WriteStatsCSV
// ==========================================================================

void InstProfiler::WriteStatsCSV()
{
    if (!stats_enabled_ || stats_map_.empty())
        return;

    struct StatEntry
    {
        const Symbol *sym;
        FuncStats st;
    };

    std::vector<StatEntry> rows;
    rows.reserve(stats_map_.size());
    for (const auto &kv : stats_map_)
        rows.push_back({kv.first, kv.second});

    std::sort(rows.begin(), rows.end(), [](const StatEntry &a, const StatEntry &b) {
        return a.st.self_sum_us > b.st.self_sum_us;
    });

    FILE *sf = fopen(param_stats_file_.c_str(), "w");
    if (!sf)
    {
        fprintf(stderr, "[InstProfiler] Could not open stats file '%s' for writing.\n", param_stats_file_.c_str());
        return;
    }

    fprintf(sf, "name,count,wall_sum_us,self_sum_us,wall_avg_us,self_avg_us\n");
    for (const auto &row : rows)
    {
        const std::string &name = Demangle(row.sym->name);
        const double wall_avg = row.st.wall_sum_us / static_cast<double>(row.st.count);
        const double self_avg = row.st.self_sum_us / static_cast<double>(row.st.count);
        std::string escaped_name = EscapeCsv(name);
        fprintf(sf,
                "\"%s\",%llu,%.3f,%.3f,%.3f,%.3f\n",
                escaped_name.c_str(),
                (unsigned long long)row.st.count,
                row.st.wall_sum_us,
                row.st.self_sum_us,
                wall_avg,
                self_avg);
    }
    fclose(sf);

    printf("[InstProfiler] Stats written to '%s' (%zu function(s))\n", param_stats_file_.c_str(), rows.size());
}
// End of Stats.cpp
