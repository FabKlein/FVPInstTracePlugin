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
 * Title:        SymbolTable.cpp
 * Description:  SymbolTable implementation: ELF detection, nm(1) parsing, range finalization
 *
 * $Date:        22 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

#include "SymbolTable.h"

#include <algorithm> // std::sort, std::lower_bound
#include <cerrno>
#include <cstdio>  // fopen, fclose, popen, pclose
#include <cstdlib> // strtoul / strtoull
#include <cstring> // strncmp
#include <sstream> // std::istringstream

// Windows compatibility: MSVC names these with an underscore prefix.
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

// ---------------------------------------------------------------------------
// Public: Load
// ---------------------------------------------------------------------------

int SymbolTable::Load(const std::string &path, const std::string &nm_tool)
{
    symbols_.clear();

    FILE *fp = nullptr;
    bool pipe_mode = false; // true -> use pclose, false -> use fclose

    if (IsElfFile(path))
    {
        // ELF binary: run nm to extract symbols.
        printf("[ChromeTrace] ELF binary detected, running nm on '%s'\n", path.c_str());
        fp = RunNm(path, nm_tool);
        if (!fp)
        {
            fprintf(stderr,
                    "[ChromeTrace] Failed to run '%s' on '%s'. "
                    "Is the nm tool in PATH?\n",
                    nm_tool.c_str(),
                    path.c_str());
            return -1;
        }
        pipe_mode = true;
    }
    else
    {
        // Plain text nm output.
        fp = fopen(path.c_str(), "r");
        if (!fp)
        {
            fprintf(stderr, "[ChromeTrace] Cannot open symbol file '%s': %s\n", path.c_str(), strerror(errno));
            return -1;
        }
    }

    int count = ParseNmStream(fp);

    if (pipe_mode)
        pclose(fp);
    else
        fclose(fp);

    if (count <= 0)
    {
        fprintf(stderr, "[ChromeTrace] No code symbols found in '%s'.\n", path.c_str());
        return count;
    }

    // Sort and infer missing end addresses.
    FinalizeRanges();

    printf("[ChromeTrace] Loaded %d symbols from '%s'\n", count, path.c_str());
    return count;
}

// ---------------------------------------------------------------------------
// Public: FindSymbol
// ---------------------------------------------------------------------------

const Symbol *SymbolTable::FindSymbol(uint64_t pc) const
{
    // Clear Thumb bit before searching.
    pc &= ~static_cast<uint64_t>(1);

    if (symbols_.empty())
        return nullptr;

    // Fast path: check the cached last result first.
    // Most instructions stay inside the same function as the previous one.
    if (last_hit_ && pc >= last_hit_->start && (last_hit_->end == 0 || pc < last_hit_->end))
        return last_hit_;

    // Binary search: find the last symbol whose start <= pc.
    // std::upper_bound gives us the first symbol with start > pc,
    // so we step back one position.
    auto it = std::upper_bound(
        symbols_.begin(), symbols_.end(), pc, [](uint64_t pc_val, const Symbol &sym) { return pc_val < sym.start; });

    if (it == symbols_.begin())
        return nullptr; // pc is below the first symbol

    --it; // now it->start <= pc

    // Check that pc falls within [start, end).
    // end == 0 means "unknown" (last symbol, no successor to bound it).
    if (it->end != 0 && pc >= it->end)
        return nullptr;

    last_hit_ = &(*it);
    return last_hit_;
}

// ---------------------------------------------------------------------------
// Private: ParseNmStream
// ---------------------------------------------------------------------------

int SymbolTable::ParseNmStream(FILE *fp)
{
    int count = 0;
    char line_buf[4096];

    while (fgets(line_buf, sizeof(line_buf), fp) != nullptr)
    {
        // Strip trailing newline / carriage-return.
        size_t len = strlen(line_buf);
        while (len > 0 && (line_buf[len - 1] == '\n' || line_buf[len - 1] == '\r'))
            line_buf[--len] = '\0';

        if (len == 0)
            continue;

        Symbol sym;
        if (ParseNmLine(std::string(line_buf), sym))
        {
            symbols_.push_back(std::move(sym));
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Private: ParseNmLine
// ---------------------------------------------------------------------------
//
// Supports two nm output formats:
//
//   With size  (nm --print-size --size-sort):
//     "0000000000008000 0000000000000120 T main"
//      field[0]=addr    field[1]=size    field[2]=type  field[3]=name
//
//   Without size (plain nm):
//     "0000000000008000 T main"
//      field[0]=addr    field[1]=type  field[2]=name
//
// Detection: if field[1] looks like a hex number (all hex digits), it is
// the size field.
//
// Only code symbols (types T, t, W, w) are accepted.
// Symbols with a null name or starting with '$' (mapping symbols) are skipped.

bool SymbolTable::ParseNmLine(const std::string &line, Symbol &out)
{
    // Tokenise on whitespace.  Use up to 4 tokens; the rest is ignored
    // (handles hypothetical whitespace in names, though we avoid --demangle).
    std::istringstream ss(line);
    std::string tokens[4];
    int n = 0;
    while (n < 4 && ss >> tokens[n])
        ++n;

    if (n < 3)
        return false; // Not enough fields.

    // ----------------------------------------------------------------
    // Determine whether line has the "with-size" (4-field) layout.
    // A field is hex if it is non-empty and contains only [0-9a-fA-F].
    // ----------------------------------------------------------------
    auto is_hex = [](const std::string &s) -> bool {
        if (s.empty())
            return false;
        for (char c : s)
            if (!isxdigit(static_cast<unsigned char>(c)))
                return false;
        return true;
    };

    std::string addr_str, type_str, name_str;
    uint64_t size_val = 0;
    bool has_size = false;

    if (n >= 4 && is_hex(tokens[1]))
    {
        // 4-field with-size format.
        addr_str = tokens[0];
        size_val = strtoull(tokens[1].c_str(), nullptr, 16);
        type_str = tokens[2];
        name_str = tokens[3];
        has_size = true;
    }
    else
    {
        // 3-field plain format.
        addr_str = tokens[0];
        type_str = tokens[1];
        name_str = tokens[2];
    }

    // Filter on symbol type: we only want code symbols.
    if (type_str.size() != 1)
        return false;
    char type_char = type_str[0];
    if (type_char != 'T' && type_char != 't' && type_char != 'W' && type_char != 'w')
        return false;

    // Skip empty names and ARM mapping symbols ($a, $t, $d, $x, ...).
    if (name_str.empty() || name_str[0] == '$')
        return false;

    // Parse address.
    uint64_t addr = strtoull(addr_str.c_str(), nullptr, 16);

    // Clear Thumb bit (bit 0) — set in nm output for Thumb function symbols
    // but absent in the PC values reported by MTI.
    addr &= ~static_cast<uint64_t>(1);

    out.start = addr;
    out.end = has_size ? (addr + size_val) : 0; // 0 = unknown, filled later
    out.name = name_str;

    return true;
}

// ---------------------------------------------------------------------------
// Private: IsElfFile
// ---------------------------------------------------------------------------

bool SymbolTable::IsElfFile(const std::string &path)
{
    // The ELF magic is the four bytes: 0x7f 'E' 'L' 'F'
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;

    unsigned char magic[4] = {0};
    bool is_elf = (fread(magic, 1, 4, f) == 4) && (magic[0] == 0x7f) && (magic[1] == 'E') && (magic[2] == 'L') &&
        (magic[3] == 'F');
    fclose(f);
    return is_elf;
}

// ---------------------------------------------------------------------------
// Private: RunNm
// ---------------------------------------------------------------------------

FILE *SymbolTable::RunNm(const std::string &path, const std::string &nm_tool)
{
    // Build command:  nm --print-size <path>
    // We do NOT pass --size-sort: that flag silently omits symbols whose
    // st_size field is 0 (common for assembly startup code and some
    // linker-script symbols).  We sort by address ourselves in FinalizeRanges.
    // We do NOT pass --demangle so symbol names never contain spaces,
    // keeping our parser simple.  Demangling is done at display time.
    //
    // Security note: path comes from the plugin parameter set by the user
    // running the FVP, not from untrusted input.
    std::string cmd = nm_tool + " --print-size \"" + path + "\"";
    return popen(cmd.c_str(), "r");
}

// ---------------------------------------------------------------------------
// Private: FinalizeRanges
// ---------------------------------------------------------------------------

void SymbolTable::FinalizeRanges()
{
    // Sort by start address.
    std::sort(symbols_.begin(), symbols_.end(), [](const Symbol &a, const Symbol &b) { return a.start < b.start; });

    // Remove exact duplicates (same start address keep first occurrence,
    // which is typically the longer / more descriptive name from nm).
    symbols_.erase(std::unique(symbols_.begin(),
                               symbols_.end(),
                               [](const Symbol &a, const Symbol &b) { return a.start == b.start; }),
                   symbols_.end());

    // Fill in missing end addresses: use the start of the next symbol.
    // Treat end == 0 (size field absent) OR end <= start (size field was 0)
    // as "unknown" — both cases are inferred from the following symbol.
    // The last symbol gets a minimal size of 4 bytes so that an exact-match
    // on its start address still succeeds.
    for (size_t i = 0; i + 1 < symbols_.size(); ++i)
    {
        if (symbols_[i].end <= symbols_[i].start)
            symbols_[i].end = symbols_[i + 1].start;
    }
    if (!symbols_.empty() && symbols_.back().end <= symbols_.back().start)
        symbols_.back().end = symbols_.back().start + 4;
}

// ---------------------------------------------------------------------------
// Public: FindSymbolByName
// ---------------------------------------------------------------------------

const Symbol *SymbolTable::FindSymbolByName(const std::string &name) const
{
    for (const Symbol &sym : symbols_)
    {
        if (sym.name == name)
            return &sym;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Public: GetAddressRange
// ---------------------------------------------------------------------------

bool SymbolTable::GetAddressRange(uint64_t &first_addr, uint64_t &last_addr) const
{
    if (symbols_.empty())
        return false;
    // Symbols are sorted by start address after FinalizeRanges().
    first_addr = symbols_.front().start;
    // Use the last symbol's end address if known, otherwise its start.
    const Symbol &last = symbols_.back();
    last_addr = (last.end != 0) ? last.end : last.start;
    return true;
}

// End of SymbolTable.cpp
