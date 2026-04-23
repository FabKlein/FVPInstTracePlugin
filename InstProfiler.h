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
 * Title:        InstProfiler.h
 * Description:  Main plugin class: call-stack tracker, Chrome Tracing JSON emitter,
 *               code coverage reporter, and per-function statistics writer
 *
 * $Date:        22 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/* Architecture overview
 * ---------------------
 *
 *   InstProfilerFactory        InstProfiler (PluginInstance)
 *   ------------------          ----------------------------
 *   Instantiate()       -->   RegisterSimulation()
 *                               Loads SymbolTable
 *                               Subscribes INST source on every CPU
 *                             TracePC()  [called per instruction by MTI]
 *                               FindSymbol(pc)
 *                               Push/pop call stack
 *                               Emit JsonWriter::WriteCompleteEvent()
 *                             Finalize()  [Release() or SIGINT]
 *                               Flush remaining open frames
 *                               Close JSON / coverage / stats files
 *
 * Call-stack inference
 * --------------------
 * The MTI INST source fires once per retired instruction.  We watch the PC
 * and compare it to the symbol table to detect function boundaries:
 *
 *   - PC enters a NEW symbol  -> push a StackFrame (call).
 *   - PC jumps to a symbol already on the stack -> it is a return:
 *     pop and emit frames back to (but not including) the resuming function.
 *   - PC stays in current sym -> nothing (hot path, very cheap).
 *
 * This heuristic works well for code compiled with standard calling
 * conventions but may miss tail calls or longjmp.
 *
 * Parameters (-C TRACE.<instance>.<name>=<value>)
 * -------------------------
 *   symbol-file   ELF binary or nm(1) output.   Default: debug.sym
 *   output-file   Destination JSON path.         Default: (empty = no JSON)
 *   demangle      "1" to demangle C++ names.     Default: 0
 *   tid           Thread ID in the trace.        Default: 1
 *   pid           Process ID in the trace.       Default: 1
 *   nm-tool       nm executable for ELF input.   Default: nm
 *   time-scale    Divide INST_COUNT by this.     Default: 1.0
 *   start-pc      Hex address to begin tracing.  Default: (from start)
 *   stop-pc       Hex address to stop tracing.   Default: (until end)
 *   start-symbol  Symbol name to begin tracing.  Default: (from start)
 *   stop-symbol   Symbol name to stop tracing.   Default: (until end)
 *   start-count   INST_COUNT to begin tracing.   Default: 0 (from start)
 *   stop-count    INST_COUNT to stop tracing.    Default: 0 (until end)
 *
 * Start/stop semantics
 * --------------------
 * If none of the start conditions are set, tracing begins immediately.
 * If ANY start condition is set, tracing is paused until whichever fires
 * first (PC hit, symbol entered, or count reached).
 * Stop conditions work the same way: the first condition to fire ends
 * the trace, flushes all open call-stack frames, and closes the JSON.
 * Symbol names must be passed in mangled form (as nm outputs them).
 *
 * Ctrl-C / SIGINT handling
 * ------------------------
 * The plugin installs SIGINT+SIGTERM handlers.  On receipt, Finalize()
 * flushes all open stack frames and closes the JSON before the process exits.
 */

#pragma once

// MTI / ESLAPI includes
#include "MTI/ModelTraceInterface.h"
#include "MTI/PluginFactory.h"
#include "MTI/PluginInstance.h"
#include "MTI/PluginInterface.h"
#include "eslapi/CAInterfaceRegistry.h"

// Plugin modules
#include "JsonWriter.h"
#include "SymbolTable.h"

// Standard library
#include <atomic>
#include <csignal> // sigaction
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef SG_MODEL_BUILD
    #include "builddata.h"
    #define PLUGIN_VERSION FULL_VERSION_STRING
#else
    #define PLUGIN_VERSION "unreleased"
#endif

// ---------------------------------------------------------------------------
// StackFrame — one activation record on the inferred call stack
// ---------------------------------------------------------------------------

struct StackFrame
{
    const Symbol *sym;         ///< Pointer into SymbolTable (stable lifetime)
    uint64_t entry_clock;      ///< INST_COUNT when function was entered
    uint64_t callee_clock = 0; ///< total ticks spent in direct callees (for self-time)
    bool emitting = true;      ///< false for frames silently pushed before capture tracing started
};

// ---------------------------------------------------------------------------
// ChromeTrace — the plugin instance
// ---------------------------------------------------------------------------

class InstProfiler : public MTI::PluginInstance {
  public:
    InstProfiler(const char *instance_name, uint32_t num_parameters, eslapi::CADIParameterValue_t *parameter_values);

    ~InstProfiler();

    // ------------------------------------------------------------------
    // CAInterface
    // ------------------------------------------------------------------
    virtual eslapi::CAInterface *
    ObtainInterface(eslapi::if_name_t ifName, eslapi::if_rev_t minRev, eslapi::if_rev_t *actualRev) override;

    // ------------------------------------------------------------------
    // MTI::PluginInstance
    // ------------------------------------------------------------------

    /// Called once by the FVP after all plugins are loaded.
    /// Subscribes to the INST trace source on every CPU found.
    virtual eslapi::CADIReturn_t RegisterSimulation(eslapi::CAInterface *simulation) override;

    /// Called before the .so is unloaded.  Finalises and closes the JSON.
    virtual void Release() override;

    /// Returns this instance's name (set by the FVP).
    virtual const char *GetName() const override;

    // ------------------------------------------------------------------
    // ParameterInterface (same signatures as CallTrace for FVP compat)
    // These methods are NOT inherited from MTI::PluginInstance — the FVP
    // discovers them via the CAInterfaceRegistry / ObtainInterface.
    // ------------------------------------------------------------------
    virtual eslapi::CADIReturn_t GetParameterInfos(uint32_t startIndex,
                                                   uint32_t desiredNumOfParams,
                                                   uint32_t *actualNumOfParams,
                                                   eslapi::CADIParameterInfo_t *params);

    virtual eslapi::CADIReturn_t GetParameterInfo(const char *parameterName, eslapi::CADIParameterInfo_t *param);

    virtual eslapi::CADIReturn_t GetParameterValues(uint32_t parameterCount,
                                                    uint32_t *actualNumOfParamsRead,
                                                    eslapi::CADIParameterValue_t *paramValuesOut);

    virtual eslapi::CADIReturn_t SetParameterValues(uint32_t parameterCount,
                                                    eslapi::CADIParameterValue_t *parameters,
                                                    eslapi::CADIFactoryErrorMessage_t *error);

    // ------------------------------------------------------------------
    // Graceful shutdown
    // ------------------------------------------------------------------

    /// Flush all open call-stack frames and finalise the JSON file.
    /// Idempotent: safe to call multiple times.
    void Finalize();

    // ------------------------------------------------------------------
    // Error helper (prints to stderr, returns CADI error code)
    // ------------------------------------------------------------------
    eslapi::CADIReturn_t Error(const char *message) const;

  private:
    // ------------------------------------------------------------------
    // Internal parameter application
    // ------------------------------------------------------------------
    eslapi::CADIReturn_t ApplyParameters(uint32_t count, eslapi::CADIParameterValue_t *values);

    // ------------------------------------------------------------------
    // MTI per-instruction callback
    // ------------------------------------------------------------------

    /// Static C-style thunk required by the MTI C callback interface.
    static void TracePC_Thunk(void *user_data, const MTI::EventClass *event_class, const MTI::EventRecord *record);

    /// The actual per-instruction handler (called from TracePC_Thunk).
    void TracePC(const MTI::EventClass *event_class, const MTI::EventRecord *record);

    // ------------------------------------------------------------------
    // Call-stack management
    // ------------------------------------------------------------------

    /// Record a function entry: push a new frame onto the call stack.
    void PushFrame(const Symbol *sym, uint64_t clock);

    /// Emit complete events for frames above @p resume_sym (exclusive),
    /// then pop them.  If @p resume_sym is nullptr, flush everything.
    void PopUntil(const Symbol *resume_sym, uint64_t clock);

    /// Emit a single "X" event for one completed stack frame.
    void EmitFrame(const StackFrame &frame, uint64_t exit_clock);

    // ------------------------------------------------------------------
    // Symbol demangling
    // ------------------------------------------------------------------

    /// Demangle @p mangled using abi::__cxa_demangle.
    /// Returns the original name if demangling fails or is disabled.
    /// Results are cached in demangle_cache_ to avoid repeated mallocs.
    const std::string &Demangle(const std::string &mangled);

    // ------------------------------------------------------------------
    // Signal-handler installation / removal
    // ------------------------------------------------------------------
    void InstallSignalHandlers();
    void RemoveSignalHandlers();

    /// Resolve start/stop parameter strings against the symbol table.
    /// Sets tracing_active_ = false if any start condition is configured.
    /// Called once from RegisterSimulation() after the symbol table is loaded.
    void ResolveGatingConditions();

    /// Write per-function code-coverage JSON to param_coverage_file_.
    /// No-op when coverage_enabled_ is false or coverage_map_ is empty.
    /// Called from Finalize().
    void WriteCoverageJson();

    /// Write per-function self/wall-time statistics CSV to param_stats_file_.
    /// No-op when stats_enabled_ is false or stats_map_ is empty.
    /// Called from Finalize().
    void WriteStatsCSV();

    // ------------------------------------------------------------------
    // Data members — parameters
    // ------------------------------------------------------------------
    std::string param_symbol_file_ = "debug.sym";
    std::string param_output_file_ = "";
    bool param_demangle_ = false;
    int param_tid_ = 1;
    int param_pid_ = 1;
    std::string param_nm_tool_ = "nm";
    double param_time_scale_ = 1.0;

    // Start/stop gating parameters.
    // Empty string / 0 means the condition is not set.
    std::string param_start_pc_ = "";         ///< Hex address string, e.g. "0x8000"
    std::string param_stop_pc_ = "";          ///< Hex address string
    std::string param_start_symbol_ = "";     ///< Mangled symbol name
    std::string param_stop_symbol_ = "";      ///< Mangled symbol name
    uint64_t param_start_count_ = 0;          ///< INST_COUNT value; 0 = not set
    uint64_t param_stop_count_ = 0;           ///< INST_COUNT value; 0 = not set
    std::string param_capture_function_ = ""; ///< Capture one function + all callees
    std::string param_coverage_file_ = "";    ///< Path for per-function coverage JSON ("" = off)
    size_t param_max_name_len_ = 128;         ///< Max demangled name chars; 0 = unlimited
    std::string param_stats_file_ = "";       ///< Path for self/wall stats CSV ("" = off)

    // ------------------------------------------------------------------
    // Data members — runtime state
    // ------------------------------------------------------------------
    std::string instance_name_;
    eslapi::CAInterfaceRegistry interface_registry_;

    SymbolTable symbol_table_;
    JsonWriter json_writer_;

    /// Inferred call stack.  Back element = current innermost function.
    std::vector<StackFrame> call_stack_;

    /// Last seen INST_COUNT — used to compute durations when flushing
    /// remaining frames at shutdown.
    uint64_t last_clock_ = 0;

    /// Running count of complete events written to the JSON file.
    uint64_t events_emitted_ = 0;

    /// True when coverage-file is set — avoids string::empty() on every instruction.
    bool coverage_enabled_ = false;

    /// Per-symbol accumulated performance statistics.
    struct FuncStats
    {
        uint64_t count = 0;
        double wall_sum_us = 0.0;
        double self_sum_us = 0.0;
    };
    bool stats_enabled_ = false;
    std::unordered_map<const Symbol *, FuncStats> stats_map_;

    /// Per-symbol set of unique PCs retired during the trace.
    /// Only populated when coverage_enabled_ is true.
    std::unordered_map<const Symbol *, std::unordered_set<uint32_t>> coverage_map_;

    /// True once Finalize() has run.
    bool finalized_ = false;

    /// Cache of { mangled → demangled } names to avoid repeated
    /// abi::__cxa_demangle calls.
    std::unordered_map<std::string, std::string> demangle_cache_;

    // MTI field indices (resolved in RegisterSimulation)
    MTI::ValueIndex inst_pc_index_ = -1;
    MTI::ValueIndex inst_count_index_ = -1;
    size_t pc_field_size_ = 4; ///< PC field size in bytes (4 or 8); set during RegisterSimulation

    /// One EventClass per attached CPU core.
    std::vector<MTI::EventClass *> event_classes_;

    // ------------------------------------------------------------------
    // Start/stop gating — resolved at RegisterSimulation() time.
    // kNoCondition (UINT64_MAX) means the condition was not specified.
    // ------------------------------------------------------------------
    static const uint64_t kNoCondition = UINT64_MAX;

    /// True once the start condition has been met (or immediately if no
    /// start condition was set).  While false, TracePC() is a no-op.
    bool tracing_active_ = true;

    uint64_t start_pc_resolved_ = kNoCondition;    ///< PC to begin tracing
    uint64_t stop_pc_resolved_ = kNoCondition;     ///< PC to stop tracing
    const Symbol *start_sym_resolved_ = nullptr;   ///< Symbol to begin tracing
    const Symbol *stop_sym_resolved_ = nullptr;    ///< Symbol to stop tracing
    const Symbol *capture_sym_resolved_ = nullptr; ///< Symbol whose return ends the trace
    uint64_t start_count_resolved_ = kNoCondition; ///< INST_COUNT to begin
    uint64_t stop_count_resolved_ = kNoCondition;  ///< INST_COUNT to stop
};

// ---------------------------------------------------------------------------
// InstProfilerFactory — creates InstProfiler instances
// ---------------------------------------------------------------------------

class InstProfilerFactory : public MTI::PluginFactory {
  public:
    InstProfilerFactory();

    virtual eslapi::CAInterface *
    ObtainInterface(eslapi::if_name_t ifName, eslapi::if_rev_t minRev, eslapi::if_rev_t *actualRev) override;

    virtual uint32_t GetNumberOfParameters() override;

    virtual eslapi::CADIReturn_t GetParameterInfos(eslapi::CADIParameterInfo_t *list) override;

    virtual eslapi::CAInterface *Instantiate(const char *instance_name,
                                             uint32_t number_of_parameters,
                                             eslapi::CADIParameterValue_t *param_values) override;

    virtual void Release() override;

    // GetType / GetVersion are plugin-specific additions, not virtual overrides.
    virtual const char *GetType() const { return "InstProfiler"; }
    virtual const char *GetVersion() const { return PLUGIN_VERSION; }

  private:
    eslapi::CAInterfaceRegistry interface_registry_;
};
