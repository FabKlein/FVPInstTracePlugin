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
 * Title:        Lcov.cpp
 * Description:  InstProfiler::WriteLcov() — source-line coverage in LCOV
 *               format, resolved via addr2line.
 *
 * $Date:        29 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/*
 * LCOV output method
 * ------------------
 * The plugin already collects unique PC values per symbol in coverage_map_
 * (shared with coverage-file).  At shutdown this method:
 *
 *   1. Collects every unique PC across all symbols.
 *   2. Writes them as hex addresses into a temporary file.
 *   3. Invokes addr2line(1) to batch-resolve PC → source file:line.
 *   4. Groups the results by source file and aggregates hit lines.
 *   5. Writes an LCOV .info file consumable by genhtml(1).
 *
 * The resulting file can be turned into an HTML coverage report:
 *
 *   genhtml coverage.info -o coverage_html
 *   xdg-open coverage_html/index.html
 *
 * Limitations
 * -----------
 * - Requires debug info (DWARF) in the ELF passed via symbol-file.
 * - Line hit counts are 1 (hit) or 0 (not seen) — the plugin tracks
 *   unique PCs, not per-PC execution frequency.
 * - addr2line occasionally returns "??:0" for addresses without debug
 *   info; these are silently skipped.
 *
 * LCOV format reference
 * ---------------------
 *   TN:<test name>
 *   SF:<absolute source path>
 *   FN:<line>,<function name>
 *   FNDA:<call count>,<function name>
 *   FNF:<functions found>
 *   FNH:<functions hit>
 *   DA:<line>,<execution count>
 *   LF:<lines found>
 *   LH:<lines hit>
 *   end_of_record
 */

#include "InstProfiler.h"

#include <algorithm> // std::sort
#include <cstdio>    // fopen, fprintf, fclose, popen, pclose, fgets
#include <cstdlib>   // remove
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Windows compatibility: MSVC names these with an underscore prefix.
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

// ==========================================================================
// InstProfiler::WriteLcov
// ==========================================================================

void InstProfiler::WriteLcov()
{
    if (!lcov_enabled_ || coverage_map_.empty())
        return;

    // LCOV uses addr2line which requires a debug-info ELF binary.
    // Check that symbol-file is an actual ELF (not plain nm text output).
    {
        FILE *probe = fopen(param_symbol_file_.c_str(), "rb");
        if (!probe)
        {
            fprintf(stderr, "[InstProfiler] lcov-file: cannot open symbol-file '%s'.\n", param_symbol_file_.c_str());
            return;
        }
        unsigned char magic[4] = {0};
        bool is_elf = (fread(magic, 1, 4, probe) == 4) && (magic[0] == 0x7f) && (magic[1] == 'E') &&
            (magic[2] == 'L') && (magic[3] == 'F');
        fclose(probe);
        if (!is_elf)
        {
            fprintf(stderr,
                    "[InstProfiler] lcov-file: symbol-file '%s' is not an ELF binary. "
                    "addr2line requires an ELF with DWARF debug info (-g). "
                    "Set symbol-file to the ELF binary (not nm text output) to enable LCOV.\n",
                    param_symbol_file_.c_str());
            return;
        }
    }

    // ------------------------------------------------------------------
    // 1. Collect ALL PCs in covered symbol ranges (not just executed).
    //    We enumerate every possible PC (stepping by 2 for Thumb) so
    //    that addr2line can resolve both hit and unhit source lines.
    //    This gives genhtml the denominator it needs — without it,
    //    every reported line appears as "hit" and coverage is 100%.
    // ------------------------------------------------------------------
    std::set<uint64_t> executed_pcs; // PCs that were actually retired
    std::vector<uint64_t> all_pcs;   // all PCs in covered symbol ranges
    std::unordered_map<uint64_t, const Symbol *> pc_to_sym;

    for (const auto &kv : coverage_map_)
    {
        const Symbol *sym = kv.first;
        const auto &hit_set = kv.second;

        // Record executed PCs.
        for (uint32_t pc : hit_set)
        {
            executed_pcs.insert(pc);
            pc_to_sym[pc] = sym;
        }

        // Enumerate the full symbol range (every 2 bytes — works for
        // both Thumb-16 and Thumb-32; ARM-32 simply has gaps that
        // addr2line maps to the same line, which is harmless).
        if (sym->end > sym->start)
        {
            for (uint64_t addr = sym->start; addr < sym->end; addr += 2)
            {
                pc_to_sym[addr] = sym;
            }
        }
    }

    // Deduplicate: merge executed PCs + range PCs into one sorted list.
    {
        std::set<uint64_t> all_set;
        for (const auto &kv : pc_to_sym)
            all_set.insert(kv.first);
        all_pcs.assign(all_set.begin(), all_set.end());
    }

    if (all_pcs.empty())
        return;

    printf("[InstProfiler] Resolving %zu unique PC(s) via '%s' for LCOV output...\n",
           all_pcs.size(),
           param_addr2line_tool_.c_str());

    // ------------------------------------------------------------------
    // 2. Write addresses to a temporary file for addr2line input.
    // ------------------------------------------------------------------
    std::string tmp_path = param_lcov_file_ + ".addr_input";
    FILE *tmp = fopen(tmp_path.c_str(), "w");
    if (!tmp)
    {
        fprintf(stderr, "[InstProfiler] Could not create temp file '%s'.\n", tmp_path.c_str());
        return;
    }
    for (uint64_t pc : all_pcs)
        fprintf(tmp, "0x%llx\n", (unsigned long long)pc);
    fclose(tmp);

    // ------------------------------------------------------------------
    // 3. Run addr2line in batch mode, reading output via popen.
    //    The -a flag prints the address before each result so we can
    //    correlate output lines even when addr2line reorders or merges.
    //    Output per address (with -a):
    //      0x00001234
    //      /path/to/file.c:42
    // ------------------------------------------------------------------
    // Shell-quote the symbol-file path to handle spaces.
    // On Windows cmd.exe only recognises double quotes; on POSIX both work.
#ifdef _WIN32
    std::string cmd =
        param_addr2line_tool_ + " -e \"" + param_symbol_file_ + "\" -a < \"" + tmp_path + "\"";
#else
    std::string cmd = param_addr2line_tool_ + " -e '" + param_symbol_file_ + "' -a < '" + tmp_path + "'";
#endif
    FILE *proc = popen(cmd.c_str(), "r");
    if (!proc)
    {
        fprintf(stderr, "[InstProfiler] Failed to run addr2line: %s\n", cmd.c_str());
        remove(tmp_path.c_str());
        return;
    }

    // Parse addr2line output: two lines per address
    //   "0x00001234"          (address echo from -a)
    //   "/path/file.c:42"    (file:line, or "??:0" / "??:?")
    struct LineInfo
    {
        std::string file;
        int line;
        uint64_t pc;
        bool hit; // true if PC was actually executed
    };
    std::vector<LineInfo> resolved;
    resolved.reserve(all_pcs.size());

    char buf[4096];
    while (fgets(buf, sizeof(buf), proc))
    {
        // First line: address (starts with "0x")
        std::string addr_line(buf);
        // Strip trailing whitespace
        while (!addr_line.empty() && (addr_line.back() == '\n' || addr_line.back() == '\r'))
            addr_line.pop_back();

        // Second line: file:line
        if (!fgets(buf, sizeof(buf), proc))
            break;
        std::string file_line(buf);
        while (!file_line.empty() && (file_line.back() == '\n' || file_line.back() == '\r'))
            file_line.pop_back();

        // Parse address
        uint64_t pc = strtoull(addr_line.c_str(), nullptr, 0);

        // Parse file:line — find last ':' to split
        size_t colon = file_line.rfind(':');
        if (colon == std::string::npos)
            continue;

        std::string file = file_line.substr(0, colon);
        std::string line_str = file_line.substr(colon + 1);

        // Skip unresolved addresses
        if (file == "??" || file.empty())
            continue;
        // Skip discriminator suffixes: "42 (discriminator 1)"
        int line_num = static_cast<int>(strtol(line_str.c_str(), nullptr, 10));
        if (line_num <= 0)
            continue;

        resolved.push_back({file, line_num, pc, executed_pcs.count(pc) > 0});
    }
    pclose(proc);
    remove(tmp_path.c_str());

    if (resolved.empty())
    {
        fprintf(stderr,
                "[InstProfiler] addr2line resolved 0 addresses. "
                "Ensure the ELF in symbol-file contains DWARF debug info (-g).\n");
        return;
    }

    printf("[InstProfiler] addr2line resolved %zu / %zu addresses to source lines.\n", resolved.size(), all_pcs.size());

    // ------------------------------------------------------------------
    // 4. Group by source file, collect hit/unhit lines and functions.
    // ------------------------------------------------------------------
    struct FileRecord
    {
        // line number → true if at least one executed PC mapped here
        std::map<int, bool> lines;
        // function name → {lowest line, was_hit}
        struct FnInfo
        {
            int line;
            bool hit;
        };
        std::map<std::string, FnInfo> functions;
    };
    // Use std::map for deterministic file ordering.
    std::map<std::string, FileRecord> file_map;

    for (const auto &li : resolved)
    {
        FileRecord &fr = file_map[li.file];

        // A line is "hit" if ANY resolved PC on that line was executed.
        auto it = fr.lines.find(li.line);
        if (it == fr.lines.end())
            fr.lines[li.line] = li.hit;
        else if (li.hit)
            it->second = true; // upgrade from unhit to hit

        // Add function info from the symbol table reverse map.
        auto sym_it = pc_to_sym.find(li.pc);
        if (sym_it != pc_to_sym.end())
        {
            const std::string &fname = Demangle(sym_it->second->name);
            auto fn_it = fr.functions.find(fname);
            if (fn_it == fr.functions.end())
                fr.functions[fname] = {li.line, li.hit};
            else
            {
                if (li.line < fn_it->second.line)
                    fn_it->second.line = li.line;
                if (li.hit)
                    fn_it->second.hit = true;
            }
        }
    }

    // ------------------------------------------------------------------
    // 5. Write LCOV output file.
    // ------------------------------------------------------------------
    FILE *lf = fopen(param_lcov_file_.c_str(), "w");
    if (!lf)
    {
        fprintf(stderr, "[InstProfiler] Could not open LCOV file '%s' for writing.\n", param_lcov_file_.c_str());
        return;
    }

    fprintf(lf, "TN:InstProfiler\n");

    size_t total_files = 0;
    size_t total_lines_hit = 0;

    for (const auto &fkv : file_map)
    {
        const std::string &src_file = fkv.first;
        const FileRecord &fr = fkv.second;

        fprintf(lf, "SF:%s\n", src_file.c_str());

        // Function records
        size_t fn_hit = 0;
        for (const auto &fn : fr.functions)
        {
            fprintf(lf, "FN:%d,%s\n", fn.second.line, fn.first.c_str());
            fprintf(lf, "FNDA:%d,%s\n", fn.second.hit ? 1 : 0, fn.first.c_str());
            if (fn.second.hit)
                ++fn_hit;
        }
        fprintf(lf, "FNF:%zu\n", fr.functions.size());
        fprintf(lf, "FNH:%zu\n", fn_hit);

        // Line data: hit lines get count 1, unhit lines get count 0.
        size_t lines_found = 0;
        size_t lines_hit = 0;
        for (const auto &lp : fr.lines)
        {
            fprintf(lf, "DA:%d,%d\n", lp.first, lp.second ? 1 : 0);
            ++lines_found;
            if (lp.second)
                ++lines_hit;
        }

        fprintf(lf, "LF:%zu\n", lines_found);
        fprintf(lf, "LH:%zu\n", lines_hit);
        fprintf(lf, "end_of_record\n");

        ++total_files;
        total_lines_hit += lines_hit;
    }

    fclose(lf);

    printf("[InstProfiler] LCOV coverage written to '%s' (%zu file(s), %zu line(s) hit)\n",
           param_lcov_file_.c_str(),
           total_files,
           total_lines_hit);
}
// End of Lcov.cpp
