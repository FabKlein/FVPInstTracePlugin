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
 * Title:        JsonWriter.cpp
 * Description:  Streaming Chrome Tracing JSON writer: file I/O and signal-safe emergency close
 *
 * $Date:        22 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

#include "JsonWriter.h"

#include <cerrno>
#include <cstdio>  // fprintf, fflush, fclose
#include <cstring> // strlen

// ---------------------------------------------------------------------------
// Destructor — guarantee the file is always properly closed
// ---------------------------------------------------------------------------

JsonWriter::~JsonWriter()
{
    // Finalize() is idempotent; always call it to be safe.
    Finalize();
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

bool JsonWriter::Open(const std::string &path)
{
    if (file_)
    {
        fprintf(stderr, "[ChromeTrace] JsonWriter::Open() called while already open.\n");
        return false;
    }

    file_ = fopen(path.c_str(), "w");
    if (!file_)
    {
        fprintf(stderr, "[ChromeTrace] Cannot open output file '%s': %s\n", path.c_str(), strerror(errno));
        return false;
    }

    // Keep a raw fd for the signal-safe EmergencyClose path.
    fd_ = fileno(file_);

    // Begin JSON array on its own line for readability.
    fprintf(file_, "[\n");
    fflush(file_);

    first_event_ = true;
    finalized_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// Finalize — normal (non-signal) close
// ---------------------------------------------------------------------------

void JsonWriter::Finalize()
{
    if (finalized_ || !file_)
        return;

    finalized_ = true;

    // Close the JSON array.  The newline before ']' keeps the file
    // grep-friendly even when opened in a text editor.
    fprintf(file_, "\n]\n");
    fflush(file_);
    fclose(file_);

    file_ = nullptr;
    fd_ = -1;
}

// ---------------------------------------------------------------------------
// EmergencyClose — signal-handler-safe
// ---------------------------------------------------------------------------

void JsonWriter::EmergencyClose()
{
    if (finalized_ || fd_ < 0)
        return;

    finalized_ = true;

    // write(2) and fsync(2) are listed as async-signal-safe by POSIX.
    const char tail[] = "\n]\n";
    // Ignore write errors in a signal handler — there is nothing useful we
    // can do if the write fails at this point.
    if (write(fd_, tail, sizeof(tail) - 1) < 0)
    { /* best effort */
    }
    if (fsync(fd_) < 0)
    { /* best effort */
    }

    // We deliberately do NOT call fclose() here because fclose() calls
    // malloc/free internally (for the FILE buffer), which is NOT
    // async-signal-safe.  The OS will close the fd when the process exits.
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void JsonWriter::WriteSeparator()
{
    if (!first_event_)
        fprintf(file_, ",\n");
    first_event_ = false;
}

// ---------------------------------------------------------------------------
// WriteCompleteEvent
// ---------------------------------------------------------------------------

void JsonWriter::WriteCompleteEvent(const std::string &name,
                                    const std::string &category,
                                    double timestamp_us,
                                    double duration_us,
                                    int pid,
                                    int tid)
{
    if (!file_ || finalized_)
        return;

    WriteSeparator();

    // Chrome Tracing "X" (Complete) event.
    // ts and dur are in microseconds (floating point for sub-microsecond res).
    fprintf(file_,
            "  {\"ph\":\"X\","
            "\"name\":\"%s\","
            "\"cat\":\"%s\","
            "\"ts\":%.3f,"
            "\"dur\":%.3f,"
            "\"pid\":%d,"
            "\"tid\":%d}",
            EscapeJson(name).c_str(),
            EscapeJson(category).c_str(),
            timestamp_us,
            duration_us,
            pid,
            tid);
}

// ---------------------------------------------------------------------------
// WriteInstantEvent
// ---------------------------------------------------------------------------

void JsonWriter::WriteInstantEvent(const std::string &name, const std::string &category, double timestamp_us, int pid)
{
    if (!file_ || finalized_)
        return;

    WriteSeparator();

    // Chrome Tracing "I" (Instant) event, process scope.
    fprintf(file_,
            "  {\"ph\":\"I\","
            "\"name\":\"%s\","
            "\"cat\":\"%s\","
            "\"ts\":%.3f,"
            "\"s\":\"p\","
            "\"pid\":%d}",
            EscapeJson(name).c_str(),
            EscapeJson(category).c_str(),
            timestamp_us,
            pid);
}

// ---------------------------------------------------------------------------
// EscapeJson
// ---------------------------------------------------------------------------

std::string JsonWriter::EscapeJson(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);

    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                // Control character: emit \uXXXX escape.
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

// End of JsonWriter.cpp
