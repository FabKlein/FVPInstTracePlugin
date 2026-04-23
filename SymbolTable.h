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
 * Title:        SymbolTable.h
 * Description:  Address-range symbol table: binary-search lookup from ELF or nm(1) output
 *
 * $Date:        22 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/* Loads function symbols from:
 *   a) An ELF binary -- detected automatically via the ELF magic bytes.
 *      The plugin runs the configured nm tool internally.
 *   b) An nm(1) output file -- two formats are supported:
 *        With size   (nm --print-size):  "addr  size  type  name"
 *        Without size (plain nm):        "addr  type  name"
 *
 * Only code symbols (types T, t, W, w) are loaded.
 *
 * Thumb bit (bit 0 of the address) is cleared on both symbol addresses and
 * incoming PC values, so AArch32/Thumb symbols are looked up correctly.
 *
 * After loading, symbols are sorted by start address.  FindSymbol() performs
 * a binary search so the per-instruction callback stays O(log N).
 *
 * NOTE: pass nm WITHOUT --demangle so that symbol names have no spaces.
 *       Demangling is done inside ChromeTrace using abi::__cxa_demangle.
 */

#pragma once

#include <cstdint>
#include <cstdio> // FILE*, popen
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Symbol — one contiguous address range owned by a function
// ---------------------------------------------------------------------------

struct Symbol
{
    uint64_t start;   ///< First address (Thumb bit already cleared)
    uint64_t end;     ///< One-past-last address (exclusive).  0 = unknown.
    std::string name; ///< Mangled symbol name (no spaces guaranteed)
};

// ---------------------------------------------------------------------------
// SymbolTable
// ---------------------------------------------------------------------------

class SymbolTable {
  public:
    SymbolTable() = default;
    ~SymbolTable() = default;

    // ------------------------------------------------------------------
    // Loading
    // ------------------------------------------------------------------

    /// Load symbols from @p path.
    ///
    /// \param path    Path to an ELF binary or nm output file.
    /// \param nm_tool nm executable used when @p path is an ELF binary
    ///                (e.g. "nm", "arm-none-eabi-nm", "aarch64-linux-gnu-nm").
    ///
    /// \return Number of symbols loaded (>= 0), or -1 on error.
    int Load(const std::string &path, const std::string &nm_tool = "nm");

    // ------------------------------------------------------------------
    // Query
    // ------------------------------------------------------------------

    /// Find the symbol whose address range contains @p pc.
    /// Clears the Thumb bit before searching.
    /// Returns nullptr if @p pc belongs to no known symbol.
    const Symbol *FindSymbol(uint64_t pc) const;

    /// Find the first symbol whose name matches @p name exactly (mangled).
    /// Linear scan — intended for setup-time use only, not the hot path.
    /// Returns nullptr if not found.
    const Symbol *FindSymbolByName(const std::string &name) const;

    size_t Size() const { return symbols_.size(); }
    bool IsEmpty() const { return symbols_.empty(); }

    /// Return the lowest start address and the highest end address in the table.
    /// Returns false (and leaves outputs unchanged) if the table is empty.
    bool GetAddressRange(uint64_t &first_addr, uint64_t &last_addr) const;

  private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Parse nm output (--print-size or plain) from an open FILE* pipe.
    /// Returns the number of symbols successfully parsed.
    int ParseNmStream(FILE *fp);

    /// Parse a single nm output line into @p out.
    /// Returns false if the line should be skipped (wrong type, parse error).
    static bool ParseNmLine(const std::string &line, Symbol &out);

    /// Return true if @p path begins with the ELF magic bytes.
    static bool IsElfFile(const std::string &path);

    /// Open a pipe to "nm_tool --print-size --size-sort path".
    /// The caller is responsible for pclose().
    static FILE *RunNm(const std::string &path, const std::string &nm_tool);

    /// After all symbols are inserted, sort by start address and fill in
    /// missing end addresses from the next symbol's start.
    void FinalizeRanges();

    // ------------------------------------------------------------------
    // Data
    // ------------------------------------------------------------------

    /// Sorted by Symbol::start (ascending).  Required for binary search.
    std::vector<Symbol> symbols_;

    /// One-entry cache: last symbol returned by FindSymbol().
    /// Avoids the binary search when consecutive PCs stay in the same function.
    mutable const Symbol *last_hit_ = nullptr;
};
