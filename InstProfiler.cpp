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
 * Title:        InstProfiler.cpp
 * Description:  InstProfiler plugin: MTI callback, call-stack tracking,
 *               Chrome Tracing JSON, coverage, and performance statistics
 *
 * $Date:        22 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/* See InstProfiler.h for the full architectural overview and parameter list. */

#include "InstProfiler.h"

#include <algorithm> // std::copy, std::find_if
#include <atomic>
#include <cctype>  // std::tolower
#include <csignal> // sigaction, SIGINT, SIGTERM
#include <cstdio>
#include <cstdlib>  // strtol, strtod
#include <cstring>  // strncpy, strcmp
#include <cxxabi.h> // abi::__cxa_demangle  (GCC / Clang)

static bool ParseBoolParam(const std::string &val)
{
    std::string lower = val;
    for (char &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return (lower == "1" || lower == "true" || lower == "yes" || lower == "on");
}

// ==========================================================================
// CADI parameter descriptors
//
// The FVP reads these at startup to build the -C TRACE.<instance>.* help text
// and to validate user-supplied parameter names.
// ==========================================================================

typedef enum
{
    PARAM_SYMBOL_FILE = 0,    ///< Path to ELF binary or nm output
    PARAM_OUTPUT_FILE,        ///< Destination JSON path
    PARAM_DEMANGLE,           ///< "1" = demangle C++ names
    PARAM_TID,                ///< Thread ID for Chrome Tracing
    PARAM_PID,                ///< Process ID for Chrome Tracing
    PARAM_NM_TOOL,            ///< nm executable (used for ELF auto-detection)
    PARAM_TIME_SCALE,         ///< INST_COUNT divisor → microseconds
    PARAM_START_PC,           ///< Hex PC address to begin tracing (empty = from start)
    PARAM_STOP_PC,            ///< Hex PC address to end tracing   (empty = until end)
    PARAM_START_SYMBOL,       ///< Symbol name to begin tracing    (empty = from start)
    PARAM_STOP_SYMBOL,        ///< Symbol name to end tracing      (empty = until end)
    PARAM_START_COUNT,        ///< INST_COUNT to begin tracing        ("0" = from start)
    PARAM_STOP_COUNT,         ///< INST_COUNT to end tracing          ("0" = until end)
    PARAM_CAPTURE_FUNCTION,   ///< Capture a single function + callees ("" = disabled)
    PARAM_CAPTURE_OCCURRENCE, ///< Start on Nth entry for start-symbol/capture-function
    PARAM_QUIT_ON_STOP,       ///< "1" exits simulation when tracing stops
    PARAM_COVERAGE_FILE,      ///< Write per-function unique-PC coverage JSON ("" = disabled)
    PARAM_MAX_NAME_LEN,       ///< Max demangled name length (0 = unlimited)
    PARAM_STATS_FILE,         ///< Write self/wall function stats CSV ("" = disabled)
    PARAM_COUNT               ///< Sentinel — total number of parameters
} ParamID;

static const eslapi::CADIParameterInfo_t kParamInfos[PARAM_COUNT] = {
    eslapi::CADIParameterInfo_t(PARAM_SYMBOL_FILE,
                                "symbol-file",
                                eslapi::CADI_PARAM_STRING,
                                "Path to an ELF binary or nm(1) output file (nm --print-size). "
                                "If an ELF binary is detected the plugin runs nm internally.",
                                /*isRuntime=*/false,
                                0,
                                0,
                                0,
                                /*default=*/"debug.sym"),

    eslapi::CADIParameterInfo_t(PARAM_OUTPUT_FILE,
                                "output-file",
                                eslapi::CADI_PARAM_STRING,
                                "Destination file for the Chrome Tracing JSON output. "
                                "Leave empty (default) to skip JSON output entirely — "
                                "useful when only coverage or statistics output is needed.",
                                false,
                                0,
                                0,
                                0,
                                ""),

    eslapi::CADIParameterInfo_t(PARAM_DEMANGLE,
                                "demangle",
                                eslapi::CADI_PARAM_STRING,
                                "Set to '1' to demangle C++ symbol names using abi::__cxa_demangle. "
                                "Default: 0 (disabled).",
                                false,
                                0,
                                0,
                                0,
                                "0"),

    eslapi::CADIParameterInfo_t(PARAM_TID,
                                "tid",
                                eslapi::CADI_PARAM_STRING,
                                "Thread ID written into every Chrome Tracing event. "
                                "Use different TIDs when running several CPUs. Default: 1.",
                                false,
                                0,
                                0,
                                0,
                                "1"),

    eslapi::CADIParameterInfo_t(PARAM_PID,
                                "pid",
                                eslapi::CADI_PARAM_STRING,
                                "Process ID written into every Chrome Tracing event. Default: 1.",
                                false,
                                0,
                                0,
                                0,
                                "1"),

    eslapi::CADIParameterInfo_t(PARAM_NM_TOOL,
                                "nm-tool",
                                eslapi::CADI_PARAM_STRING,
                                "nm executable used when the symbol-file is an ELF binary. "
                                "Use e.g. 'arm-none-eabi-nm' for bare-metal binaries. Default: nm.",
                                false,
                                0,
                                0,
                                0,
                                "nm"),

    eslapi::CADIParameterInfo_t(PARAM_TIME_SCALE,
                                "time-scale",
                                eslapi::CADI_PARAM_STRING,
                                "Divide the raw INST_COUNT by this value to obtain the timestamp "
                                "in microseconds shown in Chrome Tracing / Perfetto. "
                                "Example: set to '1000' if 1 INST_COUNT unit ≈ 1 ns. Default: 1.0.",
                                false,
                                0,
                                0,
                                0,
                                "1.0"),

    eslapi::CADIParameterInfo_t(PARAM_START_PC,
                                "start-pc",
                                eslapi::CADI_PARAM_STRING,
                                "Hex PC address at which to begin recording (e.g. '0x8000'). "
                                "Tracing starts when the CPU retires an instruction at this address. "
                                "Empty string = start from the very first instruction (default).",
                                false,
                                0,
                                0,
                                0,
                                ""),

    eslapi::CADIParameterInfo_t(PARAM_STOP_PC,
                                "stop-pc",
                                eslapi::CADI_PARAM_STRING,
                                "Hex PC address at which to stop recording. "
                                "The trace is finalised when this PC is first retired. "
                                "Empty string = trace until end of simulation (default).",
                                false,
                                0,
                                0,
                                0,
                                ""),

    eslapi::CADIParameterInfo_t(PARAM_START_SYMBOL,
                                "start-symbol",
                                eslapi::CADI_PARAM_STRING,
                                "Mangled symbol name at which to begin recording. "
                                "Tracing starts when execution first enters this symbol. "
                                "Empty string = start from the very first instruction (default).",
                                false,
                                0,
                                0,
                                0,
                                ""),

    eslapi::CADIParameterInfo_t(PARAM_STOP_SYMBOL,
                                "stop-symbol",
                                eslapi::CADI_PARAM_STRING,
                                "Mangled symbol name at which to stop recording. "
                                "The trace is finalised when execution enters this symbol. "
                                "Empty string = trace until end of simulation (default).",
                                false,
                                0,
                                0,
                                0,
                                ""),

    eslapi::CADIParameterInfo_t(PARAM_START_COUNT,
                                "start-count",
                                eslapi::CADI_PARAM_STRING,
                                "INST_COUNT value at which to begin recording (decimal). "
                                "Tracing starts when the instruction counter reaches this value. "
                                "'0' = start from the very first instruction (default).",
                                false,
                                0,
                                0,
                                0,
                                "0"),

    eslapi::CADIParameterInfo_t(PARAM_STOP_COUNT,
                                "stop-count",
                                eslapi::CADI_PARAM_STRING,
                                "INST_COUNT value at which to stop recording (decimal). "
                                "The trace is finalised when the instruction counter reaches this value. "
                                "'0' = trace until end of simulation (default).",
                                false,
                                0,
                                0,
                                0,
                                "0"),

    eslapi::CADIParameterInfo_t(PARAM_CAPTURE_FUNCTION,
                                "capture-function",
                                eslapi::CADI_PARAM_STRING,
                                "Mangled symbol name of a function to capture in isolation. "
                                "Tracing starts on the first entry to this function and stops "
                                "automatically when the function returns. "
                                "All callees are included. Empty string = disabled (default).",
                                false,
                                0,
                                0,
                                0,
                                ""),

    eslapi::CADIParameterInfo_t(PARAM_CAPTURE_OCCURRENCE,
                                "start-occurrence",
                                eslapi::CADI_PARAM_STRING,
                                "Start tracing on the Nth entry to start-symbol or capture-function. "
                                "Use '1' for first occurrence (default), '2' for second, etc. "
                                "Ignored when symbol-based start conditions are not used.",
                                false,
                                0,
                                0,
                                0,
                                "1"),

    eslapi::CADIParameterInfo_t(PARAM_QUIT_ON_STOP,
                                "quit-on-stop",
                                eslapi::CADI_PARAM_STRING,
                                "Set to '1' to terminate the simulation process once tracing stops "
                                "due to stop-pc, stop-symbol, stop-count, or capture-function return. "
                                "Default: 0 (disabled).",
                                false,
                                0,
                                0,
                                0,
                                "0"),

    eslapi::CADIParameterInfo_t(PARAM_COVERAGE_FILE,
                                "coverage-file",
                                eslapi::CADI_PARAM_STRING,
                                "If set, write a per-function code-coverage JSON to this path at the "
                                "end of the trace.  Each entry records the number of unique program "
                                "counter values retired within the function and the function's address "
                                "range in bytes.  Empty string = disabled (default).",
                                false,
                                0,
                                0,
                                0,
                                ""),

    eslapi::CADIParameterInfo_t(PARAM_MAX_NAME_LEN,
                                "max-name-len",
                                eslapi::CADI_PARAM_STRING,
                                "Maximum length (in characters) of a demangled C++ symbol name in the "
                                "trace and coverage output.  If a demangled name exceeds this limit it is "
                                "truncated for display after demangling.  "
                                "Set to '0' for unlimited.  Default: 128.",
                                false,
                                0,
                                0,
                                0,
                                "128"),

    eslapi::CADIParameterInfo_t(PARAM_STATS_FILE,
                                "stats-file",
                                eslapi::CADI_PARAM_STRING,
                                "If set, write a per-function performance statistics CSV to this path. "
                                "Columns: name, count, wall_sum_us, self_sum_us, wall_avg_us, self_avg_us. "
                                "self_sum_us excludes time spent in callees. "
                                "Sorted by self_sum_us descending.  Empty string = disabled (default).",
                                false,
                                0,
                                0,
                                0,
                                ""),
};

// ==========================================================================
// Global signal-handling state
//
// We store a pointer to the active InstProfiler instance so that the signal
// handler can trigger a graceful shutdown.  Access is protected by an atomic
// so that concurrent writes from constructor / destructor are safe.
// ==========================================================================

static std::atomic<InstProfiler *> g_instance{nullptr};
static struct sigaction g_old_sigint;
static struct sigaction g_old_sigterm;

/// The actual UNIX signal handler.
/// Sets the shutdown flag on the InstProfiler instance and then restores +
/// re-raises the original handler so that the FVP can still terminate cleanly.
static void SignalHandler(int sig)
{
    InstProfiler *inst = g_instance.load(std::memory_order_relaxed);
    if (inst)
    {
        // Finalize() is NOT async-signal-safe (it uses fprintf/fclose), but
        // it runs fast and only once.  This is acceptable for a tracing tool.
        // The alternative (EmergencyClose on the JsonWriter) is used if we
        // ever need strict async-signal-safety.
        inst->Finalize();
    }

    // Restore the original handler and re-raise so the FVP can exit.
    if (sig == SIGINT)
        sigaction(SIGINT, &g_old_sigint, nullptr);
    else
        sigaction(SIGTERM, &g_old_sigterm, nullptr);

    raise(sig);
}

// ==========================================================================
// InstProfiler constructor / destructor
// ==========================================================================

InstProfiler::InstProfiler(const char *instance_name,
                           uint32_t num_parameters,
                           eslapi::CADIParameterValue_t *parameter_values)
    : instance_name_(instance_name ? instance_name : "InstProfiler"), interface_registry_("InstProfiler")
{
    // Expose ourselves as a PluginInstance (the FVP looks up this interface).
    interface_registry_.Register<PluginInstance>(this);

    // Apply any parameters passed at instantiation time.
    if (num_parameters > 0 && parameter_values)
        ApplyParameters(num_parameters, parameter_values);

    // Install signal handlers so Ctrl-C produces a valid JSON file.
    InstallSignalHandlers();
}

InstProfiler::~InstProfiler()
{
    RemoveSignalHandlers();
    Finalize(); // idempotent — safe even if Release() already called it
}

// ==========================================================================
// CAInterface
// ==========================================================================

eslapi::CAInterface *
InstProfiler::ObtainInterface(eslapi::if_name_t ifName, eslapi::if_rev_t minRev, eslapi::if_rev_t *actualRev)
{
    return interface_registry_.ObtainInterface(ifName, minRev, actualRev);
}

// ==========================================================================
// MTI::PluginInstance — RegisterSimulation
// ==========================================================================

eslapi::CADIReturn_t InstProfiler::RegisterSimulation(eslapi::CAInterface *ca_interface)
{
    if (!ca_interface)
        return Error("RegisterSimulation: null CAInterface pointer.");

    // ----------------------------------------------------------------
    // 1. Obtain the system-wide trace interface.
    // ----------------------------------------------------------------
    MTI::SystemTraceInterface *sti = ca_interface->ObtainPointer<MTI::SystemTraceInterface>();
    if (!sti)
        return Error("RegisterSimulation: SystemTraceInterface not available.");

    MTI::SystemTraceInterface::TraceComponentIndex num_components = sti->GetNumOfTraceComponents();
    if (num_components == 0)
        return Error("RegisterSimulation: no trace-capable components found.");

    // ----------------------------------------------------------------
    // 2. Load the symbol table.
    // ----------------------------------------------------------------
    if (symbol_table_.Load(param_symbol_file_, param_nm_tool_) <= 0)
    {
        fprintf(stderr,
                "[InstProfiler] Warning: no symbols loaded from '%s'. "
                "The output JSON will be empty.\n",
                param_symbol_file_.c_str());
        // Continue — the plugin will still produce a valid (empty) JSON file.
    }
    else
    {
        // Print the symbol address range so the user can verify it matches
        // the addresses the FVP will actually execute.
        uint64_t sym_first = 0, sym_last = 0;
        if (symbol_table_.GetAddressRange(sym_first, sym_last))
            printf("[InstProfiler] Symbol address range: [0x%llx, 0x%llx)\n",
                   (unsigned long long)sym_first,
                   (unsigned long long)sym_last);
    }

    // ----------------------------------------------------------------
    // 3. Resolve start/stop gating conditions against the symbol table.
    //    This also sets tracing_active_ = false if any start condition
    //    was configured, effectively pausing the trace until triggered.
    // ----------------------------------------------------------------
    ResolveGatingConditions();

    // ----------------------------------------------------------------
    // 4. Open the JSON output file.
    // ----------------------------------------------------------------
    if (!param_output_file_.empty())
    {
        if (!json_writer_.Open(param_output_file_))
            return Error("RegisterSimulation: failed to open output JSON file.");
        printf("[InstProfiler] Writing trace to '%s'\n", param_output_file_.c_str());
    }

    // ----------------------------------------------------------------
    // 4. Subscribe to the INST source on each CPU component.
    // ----------------------------------------------------------------
    int attached = 0;
    for (MTI::SystemTraceInterface::TraceComponentIndex i = 0; i < num_components; ++i)
    {
        eslapi::CAInterface *comp_iface = sti->GetComponentTrace(i);
        if (!comp_iface)
            continue;

        MTI::ComponentTraceInterface *cti = comp_iface->ObtainPointer<MTI::ComponentTraceInterface>();
        if (!cti)
            continue;

        // We only care about the "INST" (instruction retire) source.
        MTI::TraceSource *source = cti->GetTraceSource("INST");
        if (!source)
            continue; // most FVP sub-components (buses, mappers …) have no INST source — expected

        // ----------------------------------------------------------
        // Locate required fields: PC and INST_COUNT (for the FieldMask).
        // ----------------------------------------------------------
        const MTI::EventFieldType *field_pc = source->GetField("PC");
        if (!field_pc)
        {
            fprintf(stderr, "[InstProfiler] INST source on '%s' has no PC field.\n", sti->GetComponentTracePath(i));
            continue;
        }

        const MTI::EventFieldType *field_count = source->GetField("INST_COUNT");
        if (!field_count)
        {
            fprintf(
                stderr, "[InstProfiler] INST source on '%s' has no INST_COUNT field.\n", sti->GetComponentTracePath(i));
            continue;
        }

        // Build a field mask from FieldIndex (bit position), then create the EventClass.
        // NOTE: FieldIndex (from field->GetIndex()) is used ONLY for the mask.
        //       ValueIndex (from event_class->GetValueIndex()) is used to read
        //       values from an EventRecord — these are different numbering spaces.
        MTI::FieldMask mask = (1u << field_pc->GetIndex()) | (1u << field_count->GetIndex());

        MTI::EventClass *ec = source->CreateEventClass(mask);
        if (!ec)
        {
            fprintf(stderr, "[InstProfiler] CreateEventClass failed on '%s'.\n", sti->GetComponentTracePath(i));
            continue;
        }

        // Resolve ValueIndex AFTER creating the EventClass (per CallTrace reference).
        inst_pc_index_ = ec->GetValueIndex("PC");
        inst_count_index_ = ec->GetValueIndex("INST_COUNT");
        if (inst_pc_index_ == -1 || inst_count_index_ == -1)
        {
            fprintf(stderr, "[InstProfiler] GetValueIndex failed on '%s'.\n", sti->GetComponentTracePath(i));
            continue;
        }

        // Query PC field size to support both AArch32 (4-byte) and AArch64 (8-byte) targets.
        // The field_pc pointer is valid here; get its byte size.
        pc_field_size_ = field_pc->GetSize();
        if (pc_field_size_ != 4 && pc_field_size_ != 8)
        {
            fprintf(stderr,
                    "[InstProfiler] Unexpected PC field size %zu (expected 4 or 8) on '%s'.\n",
                    pc_field_size_,
                    sti->GetComponentTracePath(i));
            pc_field_size_ = 4; // fallback to 32-bit
        }

        // ----------------------------------------------------------
        // Register callback.
        // ----------------------------------------------------------
        MTI::Status status = ec->RegisterCallback(TracePC_Thunk, this);
        if (status != MTI::MTI_OK)
        {
            fprintf(stderr, "[InstProfiler] RegisterCallback failed on '%s'.\n", sti->GetComponentTracePath(i));
            continue;
        }

        event_classes_.push_back(ec);
        ++attached;

        printf("[InstProfiler] Attached to component '%s'\n", sti->GetComponentTracePath(i));
    }

    if (attached == 0)
        return Error("RegisterSimulation: could not attach to any CPU component.");

    // Multi-core validation: this plugin only supports single-core profiling.
    // If multiple INST sources are attached, field indices and call-stack state
    // will be corrupted by interleaved callbacks. Fail loudly rather than silently
    // produce wrong results.
    if (attached > 1)
        return Error("RegisterSimulation: multi-core profiling not supported. "
                     "This plugin attaches to every INST source but maintains only one call stack "
                     "and set of field indices. Interleaved instructions from different CPUs will "
                     "corrupt call/return inference, durations, coverage, and statistics. "
                     "Please profile one core at a time or modify the plugin to split runtime state "
                     "per core.");

    return eslapi::CADI_STATUS_OK;
}

// ==========================================================================
// MTI::PluginInstance — Release / GetName
// ==========================================================================

void InstProfiler::Release()
{
    Finalize();
    delete this;
}

const char *InstProfiler::GetName() const { return instance_name_.c_str(); }

// ==========================================================================
// Finalize — flush call stack and close JSON
// ==========================================================================

void InstProfiler::Finalize()
{
    if (finalized_)
        return;
    finalized_ = true;

    // Clear the global pointer so the signal handler won't double-call us.
    g_instance.store(nullptr, std::memory_order_relaxed);

    // Flush any open call-stack frames as zero-duration events.
    // This produces valid Perfetto / Chrome Tracing events even for
    // functions that were still executing when the trace was interrupted.
    if (!call_stack_.empty())
    {
        // Count only emitting frames; pre-capture silent frames are on the stack
        // but carry no events and are not meaningful to the user.
        size_t open_count = 0;
        for (const auto &f : call_stack_)
            if (f.emitting)
                ++open_count;
        if (open_count > 0)
            printf("[InstProfiler] Flushing %zu open call-stack frame(s) at shutdown.\n", open_count);
        PopUntil(/*resume_sym=*/nullptr, last_clock_);
    }

    if (!param_output_file_.empty())
        json_writer_.Finalize();
    if (!param_output_file_.empty())
        printf("[InstProfiler] Trace written to '%s' (%llu event(s))\n",
               param_output_file_.c_str(),
               (unsigned long long)events_emitted_);

    WriteCoverageJson();
    WriteStatsCSV();
}

// ==========================================================================
// Parameter interface
// ==========================================================================

eslapi::CADIReturn_t InstProfiler::GetParameterInfos(uint32_t startIndex,
                                                     uint32_t desiredNumOfParams,
                                                     uint32_t *actualNumOfParams,
                                                     eslapi::CADIParameterInfo_t *params)
{
    if (!actualNumOfParams || !params)
        return eslapi::CADI_STATUS_IllegalArgument;

    uint32_t i = 0;
    for (; i < desiredNumOfParams; ++i)
    {
        if (startIndex + i >= PARAM_COUNT)
            break;
        params[i] = kParamInfos[startIndex + i];
    }
    *actualNumOfParams = i;
    return eslapi::CADI_STATUS_OK;
}

eslapi::CADIReturn_t InstProfiler::GetParameterInfo(const char *parameterName, eslapi::CADIParameterInfo_t *param)
{
    if (!parameterName || !param)
        return eslapi::CADI_STATUS_IllegalArgument;

    for (uint32_t i = 0; i < PARAM_COUNT; ++i)
    {
        if (strcmp(kParamInfos[i].name, parameterName) == 0)
        {
            *param = kParamInfos[i];
            return eslapi::CADI_STATUS_OK;
        }
    }
    return eslapi::CADI_STATUS_ArgNotSupported;
}

eslapi::CADIReturn_t
InstProfiler::GetParameterValues(uint32_t count, uint32_t *actualRead, eslapi::CADIParameterValue_t *out)
{
    if (!out)
        return eslapi::CADI_STATUS_IllegalArgument;

    // Helper: safely copy a std::string into the fixed CADI string buffer.
    // CADI_DESCRIPTION_SIZE is 1024 bytes (including the null terminator).
    // CADI_DESCRIPTION_SIZE == 1024 (from eslapi/CADITypes.h)
    static const size_t kBufSize = 1024;
    auto copy_str = [](char *dst, const std::string &src) {
        strncpy(dst, src.c_str(), kBufSize - 1);
        dst[kBufSize - 1] = '\0';
    };

    for (uint32_t i = 0; i < count; ++i)
    {
        switch (out[i].parameterID)
        {
        case PARAM_SYMBOL_FILE:
            copy_str(out[i].stringValue, param_symbol_file_);
            break;
        case PARAM_OUTPUT_FILE:
            copy_str(out[i].stringValue, param_output_file_);
            break;
        case PARAM_DEMANGLE:
            copy_str(out[i].stringValue, param_demangle_ ? "1" : "0");
            break;
        case PARAM_TID:
            snprintf(out[i].stringValue, kBufSize, "%d", param_tid_);
            break;
        case PARAM_PID:
            snprintf(out[i].stringValue, kBufSize, "%d", param_pid_);
            break;
        case PARAM_NM_TOOL:
            copy_str(out[i].stringValue, param_nm_tool_);
            break;
        case PARAM_TIME_SCALE:
            snprintf(out[i].stringValue, kBufSize, "%g", param_time_scale_);
            break;
        case PARAM_START_PC:
            copy_str(out[i].stringValue, param_start_pc_);
            break;
        case PARAM_STOP_PC:
            copy_str(out[i].stringValue, param_stop_pc_);
            break;
        case PARAM_START_SYMBOL:
            copy_str(out[i].stringValue, param_start_symbol_);
            break;
        case PARAM_STOP_SYMBOL:
            copy_str(out[i].stringValue, param_stop_symbol_);
            break;
        case PARAM_START_COUNT:
            snprintf(out[i].stringValue, kBufSize, "%llu", (unsigned long long)param_start_count_);
            break;
        case PARAM_STOP_COUNT:
            snprintf(out[i].stringValue, kBufSize, "%llu", (unsigned long long)param_stop_count_);
            break;
        case PARAM_CAPTURE_FUNCTION:
            copy_str(out[i].stringValue, param_capture_function_);
            break;
        case PARAM_CAPTURE_OCCURRENCE:
            snprintf(out[i].stringValue, kBufSize, "%llu", (unsigned long long)param_capture_occurrence_);
            break;
        case PARAM_QUIT_ON_STOP:
            copy_str(out[i].stringValue, param_quit_on_stop_ ? "1" : "0");
            break;
        case PARAM_COVERAGE_FILE:
            copy_str(out[i].stringValue, param_coverage_file_);
            break;
        case PARAM_MAX_NAME_LEN:
            snprintf(out[i].stringValue, kBufSize, "%zu", param_max_name_len_);
            break;
        case PARAM_STATS_FILE:
            copy_str(out[i].stringValue, param_stats_file_);
            break;
        default:
            if (actualRead)
                *actualRead = i;
            return eslapi::CADI_STATUS_IllegalArgument;
        }
    }

    if (actualRead)
        *actualRead = count;
    return eslapi::CADI_STATUS_OK;
}

eslapi::CADIReturn_t InstProfiler::SetParameterValues(uint32_t count,
                                                      eslapi::CADIParameterValue_t *values,
                                                      eslapi::CADIFactoryErrorMessage_t * /*error*/)
{
    if (!values)
        return eslapi::CADI_STATUS_IllegalArgument;
    return ApplyParameters(count, values);
}

// ---------------------------------------------------------------------------
// Internal: ApplyParameters
// ---------------------------------------------------------------------------

eslapi::CADIReturn_t InstProfiler::ApplyParameters(uint32_t count, eslapi::CADIParameterValue_t *values)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        // stringValue is CADI_DESCRIPTION_SIZE (1024) bytes — always
        // null-terminated by the CADI layer.  Construct a std::string from it
        // so all subsequent handling is buffer-overflow-safe.
        const std::string val(values[i].stringValue);

        switch (values[i].parameterID)
        {
        case PARAM_SYMBOL_FILE:
            param_symbol_file_ = val;
            break;
        case PARAM_OUTPUT_FILE:
            param_output_file_ = val;
            break;
        case PARAM_DEMANGLE:
            param_demangle_ = ParseBoolParam(val);
            break;
        case PARAM_TID:
            param_tid_ = static_cast<int>(strtol(val.c_str(), nullptr, 10));
            break;
        case PARAM_PID:
            param_pid_ = static_cast<int>(strtol(val.c_str(), nullptr, 10));
            break;
        case PARAM_NM_TOOL:
            param_nm_tool_ = val;
            break;
        case PARAM_TIME_SCALE:
            param_time_scale_ = strtod(val.c_str(), nullptr);
            if (param_time_scale_ <= 0.0)
                param_time_scale_ = 1.0;
            break;
        case PARAM_START_PC:
            param_start_pc_ = val;
            break;
        case PARAM_STOP_PC:
            param_stop_pc_ = val;
            break;
        case PARAM_START_SYMBOL:
            param_start_symbol_ = val;
            break;
        case PARAM_STOP_SYMBOL:
            param_stop_symbol_ = val;
            break;
        case PARAM_START_COUNT:
            param_start_count_ = strtoull(val.c_str(), nullptr, 10);
            break;
        case PARAM_STOP_COUNT:
            param_stop_count_ = strtoull(val.c_str(), nullptr, 10);
            break;
        case PARAM_CAPTURE_FUNCTION:
            param_capture_function_ = val;
            break;
        case PARAM_CAPTURE_OCCURRENCE:
            param_capture_occurrence_ = strtoull(val.c_str(), nullptr, 10);
            if (param_capture_occurrence_ == 0)
                param_capture_occurrence_ = 1;
            break;
        case PARAM_QUIT_ON_STOP:
            param_quit_on_stop_ = ParseBoolParam(val);
            break;
        case PARAM_COVERAGE_FILE:
            param_coverage_file_ = val;
            coverage_enabled_ = !val.empty();
            break;
        case PARAM_MAX_NAME_LEN:
            param_max_name_len_ = static_cast<size_t>(strtoull(val.c_str(), nullptr, 10));
            break;
        case PARAM_STATS_FILE:
            param_stats_file_ = val;
            stats_enabled_ = !val.empty();
            break;
        default:
            fprintf(stderr, "[InstProfiler] Unknown parameter ID %u\n", values[i].parameterID);
            break;
        }
    }
    return eslapi::CADI_STATUS_OK;
}

// ==========================================================================
// MTI callback — hot path
// ==========================================================================

/// Static thunk: the MTI C callback interface requires a plain function
/// pointer.  We route it through this static method to the instance method.
/*static*/
void InstProfiler::TracePC_Thunk(void *user_data, const MTI::EventClass *event_class, const MTI::EventRecord *record)
{
    static_cast<InstProfiler *>(user_data)->TracePC(event_class, record);
}

void InstProfiler::TracePC(const MTI::EventClass *event_class, const MTI::EventRecord *record)
{
    // ----------------------------------------------------------------
    // 0. Check for pending shutdown (set by SIGINT handler).
    //    Finalize() is idempotent, so this early-exit is safe.
    // ----------------------------------------------------------------
    if (finalized_)
        return;

    // ----------------------------------------------------------------
    // 1. Read PC and instruction count from the trace record.
    //
    //    PC field size depends on the target: AArch32 uses 4 bytes,
    //    AArch64 uses 8 bytes.  We query the actual field size during
    //    RegisterSimulation (pc_field_size_) to correctly read both.
    //    We clear bit 0 to normalise Thumb addresses.
    // ----------------------------------------------------------------
    uint64_t pc;
    if (pc_field_size_ == 8)
    {
        // 64-bit target (AArch64)
        pc = record->Get<uint64_t>(event_class, inst_pc_index_);
    }
    else
    {
        // 32-bit target (AArch32 / Cortex-M); read as uint32_t and zero-extend
        const uint32_t raw_pc32 = record->Get<uint32_t>(event_class, inst_pc_index_);
        pc = static_cast<uint64_t>(raw_pc32);
    }
    pc &= ~static_cast<uint64_t>(1); // clear Thumb bit
    const uint64_t inst_count = record->Get<uint64_t>(event_class, inst_count_index_);

    last_clock_ = inst_count;

    // ----------------------------------------------------------------
    // 1.5  Start gate: if waiting for a start condition, check all three
    //      trigger types.  Once fired, tracing_active_ is set permanently.
    // ----------------------------------------------------------------
    if (!tracing_active_)
    {
        // In capture mode: silently maintain the call stack so that parent frames
        // exist when the capture function eventually returns to them.
        // Without this, returning from the capture function looks like a new call
        // (the caller is not in the stack) and the capture frame is never popped
        // until simulation ends — producing a grossly inflated duration.
        if (capture_sym_resolved_ != nullptr)
        {
            const Symbol *pre_sym = symbol_table_.FindSymbol(pc);
            if (pre_sym && (call_stack_.empty() || call_stack_.back().sym != pre_sym))
            {
                // Check if it's a return to a frame already on the silent stack.
                const Symbol *resume_pre = nullptr;
                for (int i = static_cast<int>(call_stack_.size()) - 1; i >= 0; --i)
                {
                    if (call_stack_[static_cast<size_t>(i)].sym == pre_sym)
                    {
                        resume_pre = pre_sym;
                        break;
                    }
                }
                if (resume_pre)
                {
                    // Silent pop: remove frames above the resumed symbol.
                    while (!call_stack_.empty() && call_stack_.back().sym != resume_pre)
                        call_stack_.pop_back();
                }
                else
                {
                    // Silent push: mark as non-emitting so no JSON event is written.
                    call_stack_.push_back({pre_sym, inst_count, /*callee_clock=*/0, /*emitting=*/false});
                }
            }
        }

        bool start = false;

        // PC-based start: exact address match.
        if (start_pc_resolved_ != kNoCondition && pc == start_pc_resolved_)
            start = true;

        // Count-based start: instruction counter has reached the threshold.
        if (!start && start_count_resolved_ != kNoCondition && inst_count >= start_count_resolved_)
            start = true;

        // Symbol-based start: PC falls inside the target symbol's range.
        if (!start && start_sym_resolved_ != nullptr)
        {
            const Symbol *sym_check = symbol_table_.FindSymbol(pc);

            // Count symbol ENTRY edges, not instructions. This allows choosing
            // the Nth invocation when a symbol executes multiple times.
            const bool entered_target = (sym_check == start_sym_resolved_ && last_wait_symbol_ != start_sym_resolved_);
            if (entered_target)
            {
                ++start_symbol_seen_count_;
                if (start_symbol_seen_count_ >= param_capture_occurrence_)
                {
                    start = true;
                }
                else
                {
                    printf("[InstProfiler] Start symbol seen (%llu/%llu) — waiting for requested occurrence.\n",
                           (unsigned long long)start_symbol_seen_count_,
                           (unsigned long long)param_capture_occurrence_);
                }
            }

            last_wait_symbol_ = sym_check;
        }

        if (!start)
            return; // still waiting

        printf("[InstProfiler] Tracing started at PC=0x%llx count=%llu\n",
               (unsigned long long)pc,
               (unsigned long long)inst_count);
        tracing_active_ = true;

        if (capture_sym_resolved_ != nullptr)
        {
            // The capture function was silently pushed with emitting=false.
            // Upgrade it so its complete event is written to the JSON.
            if (!call_stack_.empty() && call_stack_.back().sym == capture_sym_resolved_)
                call_stack_.back().emitting = true;
            // Keep all parent frames — they enable correct return detection.
        }
        else
        {
            call_stack_.clear(); // discard any partial state accumulated before start
        }
    }

    // ----------------------------------------------------------------
    // 1.6  Stop gate: check PC- and count-based stop conditions.
    //      Symbol-based stop is checked after the symbol lookup below.
    // ----------------------------------------------------------------
    if (stop_pc_resolved_ != kNoCondition && pc == stop_pc_resolved_)
    {
        printf("[InstProfiler] Tracing stopped at PC=0x%llx count=%llu\n",
               (unsigned long long)pc,
               (unsigned long long)inst_count);
        Finalize();
        ExitSimulationIfRequested("stop-pc");
        return;
    }
    if (stop_count_resolved_ != kNoCondition && inst_count >= stop_count_resolved_)
    {
        printf("[InstProfiler] Tracing stopped at count=%llu\n", (unsigned long long)inst_count);
        Finalize();
        ExitSimulationIfRequested("stop-count");
        return;
    }
    // ----------------------------------------------------------------
    const Symbol *sym = symbol_table_.FindSymbol(pc);
    if (!sym)
        return; // PC not in any known symbol — ignore

    // ----------------------------------------------------------------
    // 2.5  Symbol-based stop: stop when execution enters the stop symbol.
    // ----------------------------------------------------------------
    if (stop_sym_resolved_ != nullptr && sym == stop_sym_resolved_)
    {
        printf("[InstProfiler] Tracing stopped on symbol '%s' at count=%llu\n",
               sym->name.c_str(),
               (unsigned long long)inst_count);
        Finalize();
        ExitSimulationIfRequested("stop-symbol");
        return;
    }

    // ----------------------------------------------------------------
    // 2.6  Coverage tracking: record this PC as visited for its symbol.
    //      Gated by coverage_enabled_ (a bool set once at parameter time)
    //      so the branch is free when coverage is disabled.
    // ----------------------------------------------------------------
    if (coverage_enabled_)
        coverage_map_[sym].insert(static_cast<uint32_t>(pc));

    // ----------------------------------------------------------------
    // 3. Check whether we are still inside the current top-of-stack function.
    //    This is the hot path (the vast majority of instructions).
    // ----------------------------------------------------------------
    if (!call_stack_.empty() && call_stack_.back().sym == sym)
        return; // still in the same function — nothing to do

    // ----------------------------------------------------------------
    // 4. Determine whether this is a CALL or a RETURN.
    //
    //    Heuristic: scan the call stack from top to bottom.
    //    • If we find sym somewhere in the stack → it is a return.
    //    • If sym is not in the stack         → it is a call.
    // ----------------------------------------------------------------
    const Symbol *resume_sym = nullptr;
    for (int i = static_cast<int>(call_stack_.size()) - 1; i >= 0; --i)
    {
        if (call_stack_[static_cast<size_t>(i)].sym == sym)
        {
            resume_sym = sym;
            break;
        }
    }

    if (resume_sym)
    {
        // Return path: emit and pop all frames above sym.
        // sym itself stays on the stack (it is resuming, not ending).
        PopUntil(resume_sym, inst_count);
    }
    else
    {
        // Call path: push a new frame.
        PushFrame(sym, inst_count);
    }
}

// ==========================================================================
// Call-stack helpers
// ==========================================================================

void InstProfiler::PushFrame(const Symbol *sym, uint64_t clock) { call_stack_.push_back({sym, clock}); }

void InstProfiler::PopUntil(const Symbol *resume_sym, uint64_t clock)
{
    // Pop frames from the top until we reach resume_sym.
    // If resume_sym == nullptr, flush everything.
    while (!call_stack_.empty())
    {
        if (resume_sym != nullptr && call_stack_.back().sym == resume_sym)
            break; // The resuming frame stays on the stack.

        // Capture values before pop (reference becomes dangling after pop_back).
        const bool is_capture_frame =
            (capture_sym_resolved_ != nullptr && call_stack_.back().sym == capture_sym_resolved_);
        const uint64_t frame_entry = call_stack_.back().entry_clock;

        EmitFrame(call_stack_.back(), clock);
        call_stack_.pop_back();

        // Propagate this frame's wall ticks to its parent's callee_clock so the
        // parent can compute correct self time (wall − callee).
        if (!call_stack_.empty())
            call_stack_.back().callee_clock += (clock - frame_entry);

        // capture-function mode: the function has returned — stop here.
        if (is_capture_frame)
        {
            printf("[InstProfiler] Capture function returned — finalising trace.\n");
            Finalize();
            ExitSimulationIfRequested("capture-function return");
            return;
        }
    }
}

void InstProfiler::ExitSimulationIfRequested(const char *reason)
{
    if (!param_quit_on_stop_)
        return;

    printf("[InstProfiler] quit-on-stop enabled — terminating simulation (%s).\n", reason);

    // Raise SIGTERM so the simulator exits via its normal signal path.
    // Our signal handler will run Finalize() again, which is idempotent.
    raise(SIGTERM);
}

void InstProfiler::EmitFrame(const StackFrame &frame, uint64_t exit_clock)
{
    if (!frame.emitting)
        return; // Pre-capture silent frame: do not write to JSON.

    // Convert raw instruction counts to microseconds using the time scale.
    const double ts_us = static_cast<double>(frame.entry_clock) / param_time_scale_;
    const double dur_us = static_cast<double>(exit_clock - frame.entry_clock) / param_time_scale_;

    // Resolve display name: demangled if requested, otherwise mangled.
    const std::string &display_name = Demangle(frame.sym->name);

    if (!param_output_file_.empty())
    {
        json_writer_.WriteCompleteEvent(display_name, "arm", ts_us, dur_us, param_pid_, param_tid_);
        ++events_emitted_;
    }

    // Accumulate self/wall statistics when stats-file is configured.
    // Note: stats accumulate regardless of whether JSON output is enabled.
    if (stats_enabled_)
    {
        const uint64_t wall_ticks = exit_clock - frame.entry_clock;
        const uint64_t self_ticks = (wall_ticks >= frame.callee_clock)
            ? (wall_ticks - frame.callee_clock)
            : 0; // clamp: heuristic rounding should not go negative
        auto &st = stats_map_[frame.sym];
        ++st.count;
        st.wall_sum_us += dur_us;
        st.self_sum_us += static_cast<double>(self_ticks) / param_time_scale_;
    }
}

// ==========================================================================
// Demangling
// ==========================================================================

const std::string &InstProfiler::Demangle(const std::string &mangled)
{
    // Return the original name if demangling is disabled.
    if (!param_demangle_)
        return mangled;

    // Check the cache first.
    auto it = demangle_cache_.find(mangled);
    if (it != demangle_cache_.end())
        return it->second;

    // Attempt C++ demangling using the ABI runtime.
    // abi::__cxa_demangle returns a malloc()-allocated string on success,
    // or nullptr if the name is not a valid mangled C++ name.
    int status = 0;
    char *result = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);

    std::string demangled;
    if (status == 0 && result != nullptr)
    {
        demangled = result;
        free(result); // must free the buffer returned by __cxa_demangle
    }
    else
    {
        // Not a C++ mangled name (or demangling failed) — use as-is.
        demangled = mangled;
    }

    // If the demangled name exceeds the configured limit, truncate it for
    // display after demangling. This preserves readable C++ names instead of
    // falling back to the original mangled spelling.
    if (param_max_name_len_ > 0 && demangled.size() > param_max_name_len_)
    {
        if (param_max_name_len_ <= 3)
            demangled.resize(param_max_name_len_);
        else
            demangled = demangled.substr(0, param_max_name_len_ - 3) + "...";
    }

    // Store in cache and return a reference to the cached value.
    // (C++14 compatible: no structured bindings)
    auto ins_result = demangle_cache_.emplace(mangled, std::move(demangled));
    return ins_result.first->second;
}

// ==========================================================================
// Signal handler installation / removal
// ==========================================================================

void InstProfiler::InstallSignalHandlers()
{
    // Register this instance as the target of the signal handler.
    g_instance.store(this, std::memory_order_relaxed);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART: we want system calls to be interrupted

    sigaction(SIGINT, &sa, &g_old_sigint);
    sigaction(SIGTERM, &sa, &g_old_sigterm);
}

void InstProfiler::RemoveSignalHandlers()
{
    // Only restore handlers if this instance is still the registered one.
    InstProfiler *expected = this;
    if (g_instance.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed))
    {
        sigaction(SIGINT, &g_old_sigint, nullptr);
        sigaction(SIGTERM, &g_old_sigterm, nullptr);
    }
}

// ==========================================================================
// ResolveGatingConditions
//
// Called once from RegisterSimulation() after the symbol table is loaded.
// Translates raw parameter strings into resolved runtime values and sets
// the initial state of tracing_active_.
// ==========================================================================

void InstProfiler::ResolveGatingConditions()
{
    // ----------------------------------------------------------------
    // PC-based conditions: accept "0x..." or plain decimal strings.
    // strtoull with base 0 handles both automatically.
    // Thumb bit is cleared so that addresses match MTI PC values.
    // ----------------------------------------------------------------
    if (!param_start_pc_.empty())
    {
        start_pc_resolved_ = strtoull(param_start_pc_.c_str(), nullptr, 0) & ~static_cast<uint64_t>(1);
        printf("[InstProfiler] Start condition: PC = 0x%llx\n", (unsigned long long)start_pc_resolved_);
    }
    if (!param_stop_pc_.empty())
    {
        stop_pc_resolved_ = strtoull(param_stop_pc_.c_str(), nullptr, 0) & ~static_cast<uint64_t>(1);
        printf("[InstProfiler] Stop condition: PC = 0x%llx\n", (unsigned long long)stop_pc_resolved_);
    }

    // ----------------------------------------------------------------
    // Symbol-based conditions: look up name in the loaded symbol table.
    // ----------------------------------------------------------------
    if (!param_start_symbol_.empty())
    {
        start_sym_resolved_ = symbol_table_.FindSymbolByName(param_start_symbol_);
        if (start_sym_resolved_)
            printf("[InstProfiler] Start condition: symbol '%s' @ 0x%llx\n",
                   param_start_symbol_.c_str(),
                   (unsigned long long)start_sym_resolved_->start);
        else
            fprintf(stderr,
                    "[InstProfiler] Warning: start-symbol '%s' not found in symbol table.\n",
                    param_start_symbol_.c_str());
    }
    if (!param_stop_symbol_.empty())
    {
        stop_sym_resolved_ = symbol_table_.FindSymbolByName(param_stop_symbol_);
        if (stop_sym_resolved_)
            printf("[InstProfiler] Stop condition: symbol '%s' @ 0x%llx\n",
                   param_stop_symbol_.c_str(),
                   (unsigned long long)stop_sym_resolved_->start);
        else
            fprintf(stderr,
                    "[InstProfiler] Warning: stop-symbol '%s' not found in symbol table.\n",
                    param_stop_symbol_.c_str());
    }

    // ----------------------------------------------------------------
    // Count-based conditions: 0 means "not set".
    // ----------------------------------------------------------------
    if (param_start_count_ > 0)
    {
        start_count_resolved_ = param_start_count_;
        printf("[InstProfiler] Start condition: INST_COUNT >= %llu\n", (unsigned long long)start_count_resolved_);
    }
    if (param_stop_count_ > 0)
    {
        stop_count_resolved_ = param_stop_count_;
        printf("[InstProfiler] Stop condition: INST_COUNT >= %llu\n", (unsigned long long)stop_count_resolved_);
    }
    if (param_capture_occurrence_ > 1)
    {
        printf("[InstProfiler] start-occurrence: %llu\n", (unsigned long long)param_capture_occurrence_);
    }
    if (param_quit_on_stop_)
    {
        printf("[InstProfiler] quit-on-stop: enabled\n");
    }

    // ----------------------------------------------------------------
    // capture-function: convenience shortcut that combines start-symbol
    // with "stop when that function's own frame is popped" semantics.
    // It overrides start_sym_resolved_ so the two cannot be combined.
    // ----------------------------------------------------------------
    if (!param_capture_function_.empty())
    {
        capture_sym_resolved_ = symbol_table_.FindSymbolByName(param_capture_function_);
        if (capture_sym_resolved_)
        {
            // Re-use the start-symbol machinery so TracePC() gates correctly.
            start_sym_resolved_ = capture_sym_resolved_;
            printf("[InstProfiler] Capture function: '%s' @ 0x%llx\n",
                   param_capture_function_.c_str(),
                   (unsigned long long)capture_sym_resolved_->start);
        }
        else
        {
            fprintf(stderr,
                    "[InstProfiler] Warning: capture-function '%s' not found in symbol table.\n",
                    param_capture_function_.c_str());
        }
    }

    // ----------------------------------------------------------------
    // If any start condition was specified, pause tracing until it fires.
    // Otherwise, tracing begins immediately (tracing_active_ stays true).
    // ----------------------------------------------------------------
    bool has_start_cond = (start_pc_resolved_ != kNoCondition) || (start_sym_resolved_ != nullptr) ||
        (start_count_resolved_ != kNoCondition);
    tracing_active_ = !has_start_cond;

    // Reset paused-tracing symbol edge tracking each time conditions are resolved.
    start_symbol_seen_count_ = 0;
    last_wait_symbol_ = nullptr;

    if (!tracing_active_)
        printf("[InstProfiler] Tracing is paused — waiting for start condition.\n");
}

// ==========================================================================
// Error helper
// ==========================================================================

eslapi::CADIReturn_t InstProfiler::Error(const char *message) const
{
    fprintf(stderr, "[InstProfiler] Error (%s): %s\n", instance_name_.c_str(), message);
    return eslapi::CADI_STATUS_GeneralError;
}

// ==========================================================================
// InstProfilerFactory
// ==========================================================================

InstProfilerFactory::InstProfilerFactory() : interface_registry_("InstProfilerFactory")
{
    interface_registry_.Register<MTI::PluginFactory>(this);
}

eslapi::CAInterface *
InstProfilerFactory::ObtainInterface(eslapi::if_name_t ifName, eslapi::if_rev_t minRev, eslapi::if_rev_t *actualRev)
{
    return interface_registry_.ObtainInterface(ifName, minRev, actualRev);
}

uint32_t InstProfilerFactory::GetNumberOfParameters() { return PARAM_COUNT; }

eslapi::CADIReturn_t InstProfilerFactory::GetParameterInfos(eslapi::CADIParameterInfo_t *list)
{
    if (!list)
        return eslapi::CADI_STATUS_IllegalArgument;

    std::copy(kParamInfos, kParamInfos + PARAM_COUNT, list);
    return eslapi::CADI_STATUS_OK;
}

eslapi::CAInterface *InstProfilerFactory::Instantiate(const char *instance_name,
                                                      uint32_t number_of_parameters,
                                                      eslapi::CADIParameterValue_t *param_values)
{
    return static_cast<MTI::PluginInstance *>(new InstProfiler(instance_name, number_of_parameters, param_values));
}

void InstProfilerFactory::Release()
{
    // The factory is a static instance — nothing to do.
}

// ==========================================================================
// DLL entry point — called by the FVP plugin loader
// ==========================================================================

static InstProfilerFactory factory_instance;

extern "C" {
eslapi::CAInterface *GetCAInterface() { return &factory_instance; }
}

// End of InstProfiler.cpp
